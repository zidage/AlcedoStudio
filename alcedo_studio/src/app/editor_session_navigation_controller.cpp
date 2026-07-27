//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_session_navigation_controller.hpp"

#include <utility>

#include "app/editor_mini_git_materializer.hpp"
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
  if (pending_recovery_.has_value()) {
    outcome.rejected = true;
    outcome.message  = "Resolve the failed editor save before opening another image";
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
    PendingEditorAction pending;
    pending.kind       = PendingEditorActionKind::SwitchImage;
    pending.element_id = element_id;
    pending.image_id   = image_id;
    pending.is_switch  = is_switch;
    pending.persist    = true;
    pending_action_    = pending;
    const auto ticket  = SealAndStartSave(true, true);
    if (!ticket.valid()) {
      const auto failed_pending = pending_action_.value_or(pending);
      pending_action_.reset();
      RetainPendingFailure(failed_pending, "Failed to save current image");
      outcome.failed  = true;
      outcome.message = lifecycle_.last_error();
      return outcome;
    }
    if (pending_recovery_.has_value()) {
      outcome.failed  = true;
      outcome.message = lifecycle_.last_error();
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
  if (pending_recovery_.has_value()) {
    outcome.rejected = true;
    outcome.message  = "Resolve the failed editor save before closing the editor";
    return outcome;
  }

  const auto current_identity = lifecycle_.identity();
  if (persist_changes && current_identity.element_id != 0 && current_identity.image_id != 0) {
    PendingEditorAction pending;
    pending.kind    = PendingEditorActionKind::CloseEditor;
    pending.persist = true;
    pending_action_ = pending;
    const auto ticket = SealAndStartSave(true, true);
    if (!ticket.valid()) {
      const auto failed_pending = pending_action_.value_or(pending);
      pending_action_.reset();
      RetainPendingFailure(failed_pending, "Failed to close editor session");
      outcome.failed  = true;
      outcome.message = lifecycle_.last_error();
      return outcome;
    }
    if (pending_recovery_.has_value()) {
      outcome.failed  = true;
      outcome.message = lifecycle_.last_error();
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
    // Phase 7A repair: preserve the pending target for recovery. The
    // lifecycle transitions to RetainedImageFailure so the image stays
    // visible while the user chooses Retry Save / Discard and Continue /
    // Cancel.
    RetainPendingFailure(
        pending, result.error.empty() ? "Save checkpoint failed" : result.error);
    return;
  }

  // Materializer truncates the durable journal by path. Drop the matching live
  // prefix while the history guard is still held so a same-session capture does
  // not re-include already-materialized sequences.
  if (result.last_journal_sequence.has_value() && history_ != nullptr &&
      lifecycle_.has_history_guard()) {
    std::string discard_error;
    (void)history_->DiscardMaterializedJournalThrough(
        lifecycle_.history_guard(), *result.last_journal_sequence, &discard_error);
  }

  if (pending.kind == PendingEditorActionKind::CheckoutVersion) {
    // Stay on the same image: complete the save without releasing guards, then
    // rebuild the pipeline for the requested Version.
    lifecycle_.CompleteCheckpoint();
    std::string checkout_error;
    if (ContinueCheckoutVersion(pending.version_id, &checkout_error)) {
      NotifyCompletion(true, false, "Checked out Version", pending.ticket);
    } else {
      // Phase 7A repair: the save succeeded and the history port fails closed,
      // so the prior Version is still active. Transition to
      // RetainedImageFailure to keep the image visible and signal the error
      // for recovery, rather than entering fatal Failed.
      RetainPendingFailure(
          pending, checkout_error.empty() ? "Version checkout failed" : checkout_error);
    }
    return;
  }
  if (pending.kind == PendingEditorActionKind::CreateRootVersionAndCheckout) {
    lifecycle_.CompleteCheckpoint();
    std::string create_error;
    if (ContinueCreateRootVersion(std::move(pending.display_name), &create_error)) {
      NotifyCompletion(true, false, "Created root Version", pending.ticket);
    } else {
      RetainPendingFailure(
          pending, create_error.empty() ? "Failed to create root Version" : create_error);
    }
    return;
  }
  if (pending.kind == PendingEditorActionKind::BranchFromCommitAndCheckout) {
    lifecycle_.CompleteCheckpoint();
    std::string branch_error;
    if (ContinueBranchFromCommit(pending.branch_commit, std::move(pending.display_name),
                                 &branch_error)) {
      NotifyCompletion(true, false, "Branched from commit", pending.ticket);
    } else {
      RetainPendingFailure(
          pending, branch_error.empty() ? "Failed to branch from commit" : branch_error);
    }
    return;
  }

  lifecycle_.ReleaseAfterCheckpoint();

  if (pending.kind == PendingEditorActionKind::CloseEditor) {
    ContinueToClose(pending.persist);
    NotifyCompletion(true, false, "Editor session closed", pending.ticket);
    return;
  }

  ContinueToTarget(pending.element_id, pending.image_id, pending.is_switch);
  NotifyCompletion(true, false, "Switched to next image", pending.ticket);
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
    // Project-owned global save lock is taken before capture so journal prefix
    // capture, DuckDB materialize, truncate, and thumbnail invalidation share
    // one ownership interval (Phase 2A).
    auto save_lock = save_service_.TryAcquireSaveLock(identity.element_id);
    if (!save_lock.owns_lock()) {
      return CheckpointTicket{};
    }
    // Fail closed: a configured session must have history and produce an
    // immutable capture. Missing history or a null capture is not a silent
    // successful save without materialization.
    if (history_ == nullptr || !lifecycle_.has_history_guard()) {
      return CheckpointTicket{};
    }
    std::string error;
    auto        capture = history_->CaptureSaveCheckpoint(lifecycle_.history_guard(), &error);
    if (!capture || !error.empty()) {
      return CheckpointTicket{};
    }
    if (!start_background_save) {
      return CheckpointTicket{};
    }
    SaveCheckpointRequest req;
    req.element_id         = identity.element_id;
    req.session_generation = identity.session_generation;
    req.capture            = std::move(capture);
    if (req.capture->has_journal_range()) {
      req.last_journal_sequence = req.capture->last_journal_sequence;
    }
    req.save_lock = std::move(save_lock);
    lifecycle_.BeginCheckpoint();
    return save_service_.Start(std::move(req),
                               [this](const SaveCheckpointResult& r) { OnCheckpointFinished(r); });
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

auto EditorSessionNavigationController::RequestCheckoutVersion(const version_ref_id_t& version_id)
    -> NavigationOutcome {
  std::scoped_lock  lock(mutex_);
  NavigationOutcome outcome;

  if (lifecycle_.state() == EditorSessionState::ShuttingDown) {
    outcome.rejected = true;
    outcome.message  = "Cannot checkout Version while shutting down";
    return outcome;
  }
  if (pending_action_.has_value()) {
    outcome.rejected = true;
    outcome.message  = "Editor save checkpoint is in progress";
    return outcome;
  }
  if (pending_recovery_.has_value()) {
    outcome.rejected = true;
    outcome.message  = "Resolve the failed editor save before Version checkout";
    return outcome;
  }
  if (!lifecycle_.has_history_guard() || history_ == nullptr) {
    outcome.failed  = true;
    outcome.message = "Version checkout requires an open editor history session";
    return outcome;
  }

  const auto current_identity = lifecycle_.identity();
  if (current_identity.element_id == 0 || current_identity.image_id == 0) {
    outcome.failed  = true;
    outcome.message = "Version checkout requires an open image";
    return outcome;
  }

  // Always complete a save checkpoint first (plan: checkout only for a Version
  // ref and always after save). An empty journal still acquires the global lock
  // and runs a no-op materialization path.
  PendingEditorAction pending;
  pending.kind       = PendingEditorActionKind::CheckoutVersion;
  pending.element_id = current_identity.element_id;
  pending.image_id   = current_identity.image_id;
  pending.persist    = true;
  pending.version_id = version_id;
  pending_action_    = pending;

  const auto ticket = SealAndStartSave(true, true);
  if (!ticket.valid()) {
    const auto failed_pending = pending_action_.value_or(pending);
    pending_action_.reset();
    RetainPendingFailure(failed_pending,
                         "Failed to save current image before Version checkout");
    outcome.failed  = true;
    outcome.message = lifecycle_.last_error();
    return outcome;
  }
  if (pending_recovery_.has_value()) {
    outcome.failed  = true;
    outcome.message = lifecycle_.last_error();
    return outcome;
  }
  if (!pending_action_.has_value()) {
    // Synchronous save already finished ContinueCheckoutVersion.
    outcome.ticket                    = ticket;
    outcome.sealed_session_generation = current_identity.session_generation;
    outcome.completed_synchronously   = true;
    outcome.message                   = "Checked out Version";
    return outcome;
  }
  pending_action_->ticket           = ticket;
  outcome.ticket                    = ticket;
  outcome.sealed_session_generation = current_identity.session_generation;
  outcome.waiting_for_checkpoint    = true;
  outcome.message = "Waiting for save checkpoint before Version checkout";
  return outcome;
}

auto EditorSessionNavigationController::ContinueCheckoutVersion(
    const version_ref_id_t& version_id, std::string* error) -> bool {
  if (!lifecycle_.has_history_guard() || history_ == nullptr) {
    if (error) *error = "Version checkout lost the history guard after save";
    return false;
  }

  std::string local_error;
  if (!history_->CheckoutVersion(lifecycle_.history_guard(), version_id, &local_error)) {
    // Phase 7A repair: do NOT call lifecycle_.Fail(). The history port fails
    // closed, so the prior Version and pipeline remain published. The caller
    // decides whether to keep the image Interactive or enter recovery.
    if (error) *error = local_error.empty() ? "Version checkout failed" : local_error;
    return false;
  }

  EditorRenderAdjustmentSnapshot history_snapshot;
  if (!history_->ReadAdjustmentSnapshot(lifecycle_.history_guard(), &history_snapshot,
                                        &local_error)) {
    if (error)
      *error = local_error.empty() ? "Failed to read snapshot after Version checkout"
                                   : local_error;
    return false;
  }
  if (!history_snapshot.params_json.empty() || !history_snapshot.patches.empty() ||
      !history_snapshot.fingerprint.empty()) {
    edit_.set_adjustment_snapshot(std::move(history_snapshot));
  } else {
    edit_.ClearSnapshot();
  }

  // Stay Interactive on the same image. Route a first frame for the new head
  // without releasing pipeline ownership.
  if (lifecycle_.state() != EditorSessionState::Interactive) {
    lifecycle_.MarkImageReady();
  }
  render_.ResetForNewImage();
  render_.MarkImageAcquired();

  EditorRenderCommand command;
  command.reason     = EditorRenderReason::InitialFrame;
  command.adjustment = edit_.adjustment_snapshot();
  render_.RouteInitialRender(command, lifecycle_.identity());
  return true;
}

auto EditorSessionNavigationController::ContinueCreateRootVersion(std::string display_name,
                                                                   std::string* error) -> bool {
  if (!lifecycle_.has_history_guard() || history_ == nullptr) {
    if (error) *error = "Root Version creation lost the history guard after save";
    return false;
  }

  version_ref_id_t new_version_id;
  std::string      local_error;
  if (!history_->CreateRootVersionAndCheckout(lifecycle_.history_guard(),
                                               std::move(display_name), &new_version_id,
                                               &local_error)) {
    if (error)
      *error = local_error.empty() ? "Failed to create root Version" : local_error;
    return false;
  }

  EditorRenderAdjustmentSnapshot history_snapshot;
  if (!history_->ReadAdjustmentSnapshot(lifecycle_.history_guard(), &history_snapshot,
                                        &local_error)) {
    if (error)
      *error = local_error.empty() ? "Failed to read snapshot after root Version creation"
                                   : local_error;
    return false;
  }
  if (!history_snapshot.params_json.empty() || !history_snapshot.patches.empty() ||
      !history_snapshot.fingerprint.empty()) {
    edit_.set_adjustment_snapshot(std::move(history_snapshot));
  } else {
    edit_.ClearSnapshot();
  }

  if (lifecycle_.state() != EditorSessionState::Interactive) {
    lifecycle_.MarkImageReady();
  }
  render_.ResetForNewImage();
  render_.MarkImageAcquired();

  EditorRenderCommand command;
  command.reason     = EditorRenderReason::InitialFrame;
  command.adjustment = edit_.adjustment_snapshot();
  render_.RouteInitialRender(command, lifecycle_.identity());
  return true;
}

auto EditorSessionNavigationController::ContinueBranchFromCommit(
    const commit_hash_t& commit_id, std::string display_name, std::string* error) -> bool {
  if (!lifecycle_.has_history_guard() || history_ == nullptr) {
    if (error) *error = "Branch creation lost the history guard after save";
    return false;
  }

  version_ref_id_t new_version_id;
  std::string      local_error;
  if (!history_->BranchFromCommitAndCheckout(lifecycle_.history_guard(), commit_id,
                                              std::move(display_name), &new_version_id,
                                              &local_error)) {
    if (error)
      *error = local_error.empty() ? "Failed to branch from commit" : local_error;
    return false;
  }

  EditorRenderAdjustmentSnapshot history_snapshot;
  if (!history_->ReadAdjustmentSnapshot(lifecycle_.history_guard(), &history_snapshot,
                                        &local_error)) {
    if (error)
      *error = local_error.empty() ? "Failed to read snapshot after branch creation"
                                   : local_error;
    return false;
  }
  if (!history_snapshot.params_json.empty() || !history_snapshot.patches.empty() ||
      !history_snapshot.fingerprint.empty()) {
    edit_.set_adjustment_snapshot(std::move(history_snapshot));
  } else {
    edit_.ClearSnapshot();
  }

  if (lifecycle_.state() != EditorSessionState::Interactive) {
    lifecycle_.MarkImageReady();
  }
  render_.ResetForNewImage();
  render_.MarkImageAcquired();

  EditorRenderCommand command;
  command.reason     = EditorRenderReason::InitialFrame;
  command.adjustment = edit_.adjustment_snapshot();
  render_.RouteInitialRender(command, lifecycle_.identity());
  return true;
}

void EditorSessionNavigationController::SetCompletionNotifier(
    NavigationCompletionNotifier notifier) {
  std::scoped_lock lock(mutex_);
  completion_notifier_ = std::move(notifier);
}

void EditorSessionNavigationController::RetainPendingFailure(PendingEditorAction pending,
                                                              std::string message) {
  lifecycle_.KeepCurrentAfterCheckpointFailure(std::move(message));
  pending_recovery_ = std::move(pending);
  NotifyCompletion(false, true, lifecycle_.last_error(), pending_recovery_->ticket);
}

void EditorSessionNavigationController::NotifyCompletion(bool success, bool retained_image,
                                                          std::string message,
                                                          const CheckpointTicket& ticket) {
  if (completion_notifier_) {
    NavigationCompletion completion;
    completion.success        = success;
    completion.retained_image  = retained_image;
    completion.message         = std::move(message);
    completion.ticket          = ticket;
    completion_notifier_(completion);
  }
}

auto EditorSessionNavigationController::has_pending_recovery() const -> bool {
  std::scoped_lock lock(mutex_);
  return pending_recovery_.has_value();
}

auto EditorSessionNavigationController::RetrySaveAfterFailure() -> NavigationOutcome {
  std::scoped_lock lock(mutex_);
  NavigationOutcome outcome;
  if (!pending_recovery_.has_value()) {
    outcome.rejected = true;
    outcome.message  = "No pending recovery target";
    return outcome;
  }
  if (lifecycle_.state() != EditorSessionState::RetainedImageFailure) {
    outcome.rejected = true;
    outcome.message  = "Retry Save requires a retained-image failure state";
    return outcome;
  }
  const auto recovery = *pending_recovery_;
  pending_recovery_.reset();
  // Re-attempt the save checkpoint for the same pending target.
  pending_action_    = recovery;
  pending_action_->ticket = {};
  const auto ticket = SealAndStartSave(true, true);
  if (!ticket.valid()) {
    const auto failed_pending = pending_action_.value_or(recovery);
    pending_action_.reset();
    if (!pending_recovery_.has_value()) {
      RetainPendingFailure(failed_pending, "Failed to retry save");
    }
    outcome.failed  = true;
    outcome.message = lifecycle_.last_error();
    return outcome;
  }
  if (pending_recovery_.has_value()) {
    outcome.failed  = true;
    outcome.message = lifecycle_.last_error();
    return outcome;
  }
  if (!pending_action_.has_value()) {
    outcome.ticket                    = ticket;
    outcome.sealed_session_generation = lifecycle_.identity().session_generation;
    outcome.completed_synchronously   = true;
    outcome.message                   = "Save retried";
    return outcome;
  }
  pending_action_->ticket           = ticket;
  outcome.ticket                    = ticket;
  outcome.sealed_session_generation = lifecycle_.identity().session_generation;
  outcome.waiting_for_checkpoint    = true;
  outcome.message = "Waiting for save checkpoint retry";
  return outcome;
}

auto EditorSessionNavigationController::DiscardAndContinueAfterFailure() -> NavigationOutcome {
  std::scoped_lock lock(mutex_);
  NavigationOutcome outcome;
  if (!pending_recovery_.has_value()) {
    outcome.rejected = true;
    outcome.message  = "No pending recovery target";
    return outcome;
  }
  if (lifecycle_.state() != EditorSessionState::RetainedImageFailure) {
    outcome.rejected = true;
    outcome.message  = "Discard requires a retained-image failure state";
    return outcome;
  }
  const auto recovery = *pending_recovery_;

  // Explicitly discard unflushed journal/working changes before continuing.
  if (journal_ != nullptr) {
    std::string discard_error;
    if (!journal_->DiscardUnflushed(lifecycle_.identity().element_id, &discard_error)) {
      const auto message = discard_error.empty() ? "Failed to discard pending editor changes"
                                                 : discard_error;
      RetainPendingFailure(recovery, message);
      outcome.failed  = true;
      outcome.message = lifecycle_.last_error();
      return outcome;
    }
  }
  pending_recovery_.reset();

  // Continue the pending navigation with persist=false (no save needed).
  if (recovery.kind == PendingEditorActionKind::CloseEditor) {
    render_.CancelSessionAndWait(lifecycle_.identity().session_generation);
    lifecycle_.ReleaseGuards();
    lifecycle_.CompleteClose();
    edit_.ClearSnapshot();
    render_.ResetForNewImage();
    outcome.completed_synchronously = true;
    outcome.message                  = "Editor changes discarded";
    NotifyCompletion(true, false, "Discarded and closed", recovery.ticket);
    return outcome;
  }
  if (recovery.kind == PendingEditorActionKind::CheckoutVersion) {
    // No save needed; just checkout the target Version on the current image.
    lifecycle_.ResumeInteractiveAfterFailure();
    std::string checkout_error;
    if (ContinueCheckoutVersion(recovery.version_id, &checkout_error)) {
      outcome.completed_synchronously = true;
      outcome.message                 = "Discarded and checked out Version";
      NotifyCompletion(true, false, "Discarded and checked out Version", recovery.ticket);
    } else {
      RetainPendingFailure(
          recovery, checkout_error.empty() ? "Version checkout failed after discard"
                                            : checkout_error);
      outcome.failed  = true;
      outcome.message = lifecycle_.last_error();
    }
    return outcome;
  }
  if (recovery.kind == PendingEditorActionKind::CreateRootVersionAndCheckout) {
    lifecycle_.ResumeInteractiveAfterFailure();
    std::string create_error;
    if (ContinueCreateRootVersion(std::move(recovery.display_name), &create_error)) {
      outcome.completed_synchronously = true;
      outcome.message                  = "Discarded and created root Version";
      NotifyCompletion(true, false, "Discarded and created root Version", recovery.ticket);
    } else {
      RetainPendingFailure(
          recovery, create_error.empty() ? "Root Version creation failed after discard"
                                          : create_error);
      outcome.failed  = true;
      outcome.message = lifecycle_.last_error();
    }
    return outcome;
  }
  if (recovery.kind == PendingEditorActionKind::BranchFromCommitAndCheckout) {
    lifecycle_.ResumeInteractiveAfterFailure();
    std::string branch_error;
    if (ContinueBranchFromCommit(recovery.branch_commit, std::move(recovery.display_name),
                                 &branch_error)) {
      outcome.completed_synchronously = true;
      outcome.message                  = "Discarded and branched from commit";
      NotifyCompletion(true, false, "Discarded and branched from commit", recovery.ticket);
    } else {
      RetainPendingFailure(
          recovery, branch_error.empty() ? "Branch creation failed after discard"
                                         : branch_error);
      outcome.failed  = true;
      outcome.message = lifecycle_.last_error();
    }
    return outcome;
  }
  // SwitchImage: release the current image and acquire the target.
  render_.CancelSessionAndWait(lifecycle_.identity().session_generation);
  lifecycle_.ReleaseGuards();
  edit_.ClearSnapshot();
  ContinueToTarget(recovery.element_id, recovery.image_id, recovery.is_switch);
  outcome.completed_synchronously = true;
  outcome.message                  = "Discarded and switched image";
  NotifyCompletion(true, false, "Discarded and switched image", recovery.ticket);
  return outcome;
}

void EditorSessionNavigationController::CancelPendingNavigation() {
  std::scoped_lock lock(mutex_);
  pending_recovery_.reset();
  if (lifecycle_.state() == EditorSessionState::RetainedImageFailure) {
    lifecycle_.ResumeInteractiveAfterFailure();
  }
  NotifyCompletion(true, false, "Pending navigation cancelled", {});
}

auto EditorSessionNavigationController::RequestCreateRootVersion(std::string display_name)
    -> NavigationOutcome {
  std::scoped_lock lock(mutex_);
  NavigationOutcome outcome;
  if (lifecycle_.state() == EditorSessionState::ShuttingDown) {
    outcome.rejected = true;
    outcome.message  = "Cannot create Version while shutting down";
    return outcome;
  }
  if (pending_action_.has_value()) {
    outcome.rejected = true;
    outcome.message  = "Editor save checkpoint is in progress";
    return outcome;
  }
  if (pending_recovery_.has_value()) {
    outcome.rejected = true;
    outcome.message  = "Resolve the failed editor save before creating a Version";
    return outcome;
  }
  if (!lifecycle_.has_history_guard() || history_ == nullptr) {
    outcome.failed  = true;
    outcome.message = "Root Version creation requires an open editor history session";
    return outcome;
  }
  const auto current_identity = lifecycle_.identity();
  if (current_identity.element_id == 0 || current_identity.image_id == 0) {
    outcome.failed  = true;
    outcome.message = "Root Version creation requires an open image";
    return outcome;
  }
  PendingEditorAction pending;
  pending.kind         = PendingEditorActionKind::CreateRootVersionAndCheckout;
  pending.element_id   = current_identity.element_id;
  pending.image_id     = current_identity.image_id;
  pending.persist      = true;
  pending.display_name = std::move(display_name);
  pending_action_      = pending;
  const auto ticket    = SealAndStartSave(true, true);
  if (!ticket.valid()) {
    const auto failed_pending = pending_action_.value_or(pending);
    pending_action_.reset();
    RetainPendingFailure(failed_pending,
                         "Failed to save current image before root Version creation");
    outcome.failed  = true;
    outcome.message = lifecycle_.last_error();
    return outcome;
  }
  if (pending_recovery_.has_value()) {
    outcome.failed  = true;
    outcome.message = lifecycle_.last_error();
    return outcome;
  }
  if (!pending_action_.has_value()) {
    outcome.ticket                    = ticket;
    outcome.sealed_session_generation = current_identity.session_generation;
    outcome.completed_synchronously   = true;
    outcome.message                   = "Created root Version";
    return outcome;
  }
  pending_action_->ticket           = ticket;
  outcome.ticket                    = ticket;
  outcome.sealed_session_generation = current_identity.session_generation;
  outcome.waiting_for_checkpoint    = true;
  outcome.message = "Waiting for save checkpoint before root Version creation";
  return outcome;
}

auto EditorSessionNavigationController::RequestBranchFromCommit(const commit_hash_t& commit_id,
                                                                 std::string display_name)
    -> NavigationOutcome {
  std::scoped_lock lock(mutex_);
  NavigationOutcome outcome;
  if (lifecycle_.state() == EditorSessionState::ShuttingDown) {
    outcome.rejected = true;
    outcome.message  = "Cannot branch while shutting down";
    return outcome;
  }
  if (pending_action_.has_value()) {
    outcome.rejected = true;
    outcome.message  = "Editor save checkpoint is in progress";
    return outcome;
  }
  if (pending_recovery_.has_value()) {
    outcome.rejected = true;
    outcome.message  = "Resolve the failed editor save before creating a branch";
    return outcome;
  }
  if (!lifecycle_.has_history_guard() || history_ == nullptr) {
    outcome.failed  = true;
    outcome.message = "Branch creation requires an open editor history session";
    return outcome;
  }
  const auto current_identity = lifecycle_.identity();
  if (current_identity.element_id == 0 || current_identity.image_id == 0) {
    outcome.failed  = true;
    outcome.message = "Branch creation requires an open image";
    return outcome;
  }
  PendingEditorAction pending;
  pending.kind          = PendingEditorActionKind::BranchFromCommitAndCheckout;
  pending.element_id    = current_identity.element_id;
  pending.image_id      = current_identity.image_id;
  pending.persist       = true;
  pending.branch_commit = commit_id;
  pending.display_name  = std::move(display_name);
  pending_action_       = pending;
  const auto ticket     = SealAndStartSave(true, true);
  if (!ticket.valid()) {
    const auto failed_pending = pending_action_.value_or(pending);
    pending_action_.reset();
    RetainPendingFailure(failed_pending,
                         "Failed to save current image before branch creation");
    outcome.failed  = true;
    outcome.message = lifecycle_.last_error();
    return outcome;
  }
  if (pending_recovery_.has_value()) {
    outcome.failed  = true;
    outcome.message = lifecycle_.last_error();
    return outcome;
  }
  if (!pending_action_.has_value()) {
    outcome.ticket                    = ticket;
    outcome.sealed_session_generation = current_identity.session_generation;
    outcome.completed_synchronously   = true;
    outcome.message                   = "Branched from commit";
    return outcome;
  }
  pending_action_->ticket           = ticket;
  outcome.ticket                    = ticket;
  outcome.sealed_session_generation = current_identity.session_generation;
  outcome.waiting_for_checkpoint    = true;
  outcome.message = "Waiting for save checkpoint before branch creation";
  return outcome;
}

}  // namespace alcedo
