//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_session_service.hpp"

#include <algorithm>
#include <utility>

#include "app/editor_mini_git_materializer.hpp"
#include "app/editor_session_edit_controller.hpp"
#include "app/editor_session_lifecycle.hpp"
#include "app/editor_session_navigation_controller.hpp"
#include "app/editor_session_render_controller.hpp"

namespace alcedo {

EditorSessionService::EditorSessionService(Dependencies dependencies)
    : dependencies_(std::move(dependencies)),
      lifecycle_(
          EditorSessionLifecycle::Dependencies{dependencies_.pipeline, dependencies_.history}),
      save_service_(EditorSaveCheckpointService::Dependencies{
          dependencies_.journal, dependencies_.checkpoint_store, dependencies_.thumbnails,
          dependencies_.tasks, dependencies_.save_coordinator}),
      render_(EditorSessionRenderController::Dependencies{
          dependencies_.render,
          [this](const EditorRenderEvent& event) {
            EditorSessionResult result;
            switch (event.kind) {
              case EditorRenderEventKind::FirstFramePresented: {
                // Transition lifecycle to Interactive. The render controller
                // no longer mutates lifecycle; the facade owns this transition.
                const auto entered = lifecycle_.MarkFirstFramePresented();
                if (entered.has_value()) {
                  result.kind     = EditorSessionResultKind::ImageReady;
                  result.state    = EditorSessionState::Interactive;
                  result.identity = *entered;
                } else {
                  result.kind     = EditorSessionResultKind::StateChanged;
                  result.state    = lifecycle_.state();
                  result.identity = event.identity;
                }
                result.message = event.message;
                break;
              }
              case EditorRenderEventKind::RenderFailed:
                result.kind    = EditorSessionResultKind::Failed;
                result.message = event.message;
                break;
              case EditorRenderEventKind::RenderRouted:
                result.kind              = EditorSessionResultKind::RenderRouted;
                result.render_request_id = event.request_id;
                result.message           = event.message;
                break;
              case EditorRenderEventKind::RenderReused:
                result.kind    = EditorSessionResultKind::Accepted;
                result.message = event.message;
                break;
              case EditorRenderEventKind::RenderRejected:
                result.kind    = EditorSessionResultKind::Rejected;
                result.message = event.message;
                break;
              case EditorRenderEventKind::BusyChanged:
                NotifyChange();
                return;
            }
            result.state    = event.state;
            result.identity = event.identity;
            Emit(std::move(result));
          }}),
      edit_(
          EditorSessionEditController::Dependencies{dependencies_.history, dependencies_.journal}),
      navigation_(lifecycle_, save_service_, render_, edit_, dependencies_.journal.get(),
                  dependencies_.checkpoint_store.get(), dependencies_.history.get()) {
  navigation_.SetCompletionNotifier([this](const NavigationCompletion& completion) {
    EditorSessionResult result;
    result.identity    = lifecycle_.identity();
    result.state       = lifecycle_.state();
    result.task_id     = completion.ticket.task_id;
    if (completion.success) {
      if (render_.first_frame_request_id() != 0) {
        result.kind              = EditorSessionResultKind::RenderRouted;
        result.render_request_id = render_.first_frame_request_id();
      } else {
        result.kind = EditorSessionResultKind::Accepted;
      }
      result.message = completion.message;
      BumpHistoryRevision();
    } else {
      result.kind    = EditorSessionResultKind::Failed;
      result.state   = lifecycle_.state();
      result.message = completion.message;
    }
    Emit(std::move(result));
  });
}

EditorSessionService::~EditorSessionService() { save_service_.CancelAndWait(); }

void EditorSessionService::SetResultObserver(ResultObserver observer) {
  // Observer delivery is GUI-thread serialized by the controller's install path;
  // still take results_mutex_ so concurrent Emit and SetResultObserver stay safe.
  std::scoped_lock lock(results_mutex_);
  IEditorSessionBackend::SetResultObserver(std::move(observer));
}

void EditorSessionService::SetChangeNotifier(ChangeNotifier notifier) {
  change_notifier_ = std::move(notifier);
}

auto EditorSessionService::Emit(EditorSessionResult result) -> EditorSessionResult {
  {
    std::scoped_lock lock(results_mutex_);
    results_.push_back(result);
  }
  NotifyResult(result);
  NotifyChange();
  return result;
}

auto EditorSessionService::Reject(std::string message) -> EditorSessionResult {
  // Surface rejection does not change the lifecycle state. The caller is
  // responsible for transitioning lifecycle when a rejection represents a
  // real failure (e.g. acquire or render failure).
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::Rejected;
  result.state    = lifecycle_.state();
  result.identity = lifecycle_.identity();
  result.message  = std::move(message);
  return Emit(std::move(result));
}

/// Transition lifecycle to Failed and emit a Failed result. Used when a
/// navigation or save failure requires the session to enter the Failed state.
auto EditorSessionService::Fail(std::string message) -> EditorSessionResult {
  if (lifecycle_.state() == EditorSessionState::RetainedImageFailure) {
    EditorSessionResult result;
    result.kind     = EditorSessionResultKind::Failed;
    result.state    = lifecycle_.state();
    result.identity = lifecycle_.identity();
    result.message  = std::move(message);
    return result;
  }
  lifecycle_.Fail(message);
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::Failed;
  result.state    = lifecycle_.state();
  result.identity = lifecycle_.identity();
  result.message  = std::move(message);
  BumpHistoryRevision();
  return Emit(std::move(result));
}

auto EditorSessionService::FinishVersionNavigation(const NavigationOutcome& outcome)
    -> EditorSessionResult {
  if (outcome.rejected) {
    return Reject(outcome.message);
  }
  if (outcome.failed) {
    return Fail(outcome.message);
  }
  if (outcome.waiting_for_checkpoint) {
    EditorSessionResult waiting;
    waiting.kind     = EditorSessionResultKind::SaveStarted;
    waiting.state    = lifecycle_.state();
    waiting.identity = lifecycle_.identity();
    waiting.task_id  = outcome.ticket.task_id;
    waiting.message  = outcome.message;
    return Emit(std::move(waiting));
  }
  if (outcome.ticket.valid()) {
    EditorSessionResult started;
    started.kind     = EditorSessionResultKind::SaveStarted;
    started.state    = lifecycle_.state();
    started.identity = lifecycle_.identity();
    started.task_id  = outcome.ticket.task_id;
    started.message  = "Save started";
    Emit(std::move(started));
  }

  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::Accepted;
  result.state    = lifecycle_.state();
  result.identity = lifecycle_.identity();
  result.message  = outcome.message;
  if (render_.first_frame_request_id() != 0) {
    result.kind              = EditorSessionResultKind::RenderRouted;
    result.render_request_id = render_.first_frame_request_id();
  }
  if (outcome.ticket.valid()) {
    EditorSessionResult finished;
    finished.kind     = EditorSessionResultKind::SaveFinished;
    finished.state    = lifecycle_.state();
    finished.identity = lifecycle_.identity();
    finished.task_id  = outcome.ticket.task_id;
    finished.message  = "Editor session materialized";
    Emit(std::move(finished));
  }
  BumpHistoryRevision();
  return Emit(std::move(result));
}

auto EditorSessionService::Open(sl_element_id_t element_id, image_id_t image_id)
    -> EditorSessionResult {
  std::string merge_error;
  if (!CancelPendingMergeForNavigation(&merge_error)) {
    return Reject(std::move(merge_error));
  }
  const auto outcome = navigation_.RequestOpenOrSwitch(element_id, image_id, false);
  if (outcome.rejected) {
    return Reject(outcome.message);
  }
  if (outcome.failed) {
    return Fail(outcome.message);
  }
  if (outcome.waiting_for_checkpoint) {
    EditorSessionResult waiting;
    waiting.kind     = EditorSessionResultKind::SaveStarted;
    waiting.state    = lifecycle_.state();
    waiting.identity = lifecycle_.identity();
    waiting.task_id  = outcome.ticket.task_id;
    waiting.message  = outcome.message;
    return Emit(std::move(waiting));
  }
  // Synchronous save completed. Emit SaveStarted + SaveFinished so observers
  // see the same event sequence as the async path.
  if (outcome.ticket.valid()) {
    EditorSessionResult started;
    started.kind     = EditorSessionResultKind::SaveStarted;
    started.state    = lifecycle_.state();
    started.identity = lifecycle_.identity();
    started.task_id  = outcome.ticket.task_id;
    started.message  = "Save started";
    Emit(std::move(started));
  }
  EditorSessionResult result;
  if (outcome.same_image_noop) {
    result.kind     = EditorSessionResultKind::Accepted;
    result.state    = lifecycle_.state();
    result.identity = lifecycle_.identity();
    result.message  = outcome.message;
    return Emit(std::move(result));
  }
  if (lifecycle_.state() == EditorSessionState::Failed) {
    result.kind    = EditorSessionResultKind::Failed;
    result.message = lifecycle_.last_error();
  } else if (lifecycle_.state() == EditorSessionState::Loading ||
             lifecycle_.state() == EditorSessionState::Acquiring ||
             lifecycle_.state() == EditorSessionState::Switching) {
    if (render_.first_frame_request_id() == 0 && render_.PresentationTargetReady()) {
      result.kind    = EditorSessionResultKind::Failed;
      result.message = "First frame could not be scheduled";
      lifecycle_.Fail(result.message);
    } else if (render_.first_frame_request_id() == 0) {
      result.kind = EditorSessionResultKind::StateChanged;
    } else {
      result.kind              = EditorSessionResultKind::RenderRouted;
      result.render_request_id = render_.first_frame_request_id();
    }
  } else if (lifecycle_.state() == EditorSessionState::Interactive) {
    result.kind = EditorSessionResultKind::Accepted;
  } else if (lifecycle_.state() == EditorSessionState::NoImage) {
    result.kind = EditorSessionResultKind::StateChanged;
  } else {
    result.kind = EditorSessionResultKind::Accepted;
  }
  result.state    = lifecycle_.state();
  result.identity = lifecycle_.identity();
  result.message  = outcome.message;
  // Emit a SaveFinished event when the save completed synchronously.
  if (outcome.ticket.valid()) {
    EditorSessionResult finished;
    finished.kind     = EditorSessionResultKind::SaveFinished;
    finished.state    = lifecycle_.state();
    finished.identity = lifecycle_.identity();
    finished.task_id  = outcome.ticket.task_id;
    finished.message  = "Editor session materialized";
    Emit(std::move(finished));
  }
  BumpHistoryRevision();
  return Emit(std::move(result));
}

auto EditorSessionService::CheckoutVersion(const version_ref_id_t& version_id)
    -> EditorSessionResult {
  std::string merge_error;
  if (!CancelPendingMergeForNavigation(&merge_error)) {
    return Reject(std::move(merge_error));
  }
  const auto outcome = navigation_.RequestCheckoutVersion(version_id);
  return FinishVersionNavigation(outcome);
}

auto EditorSessionService::history_snapshot() -> EditorHistorySnapshot {
  if (!dependencies_.history || !lifecycle_.has_history_guard()) return {};
  std::string           error;
  EditorHistorySnapshot snapshot;
  if (!dependencies_.history->ReadHistorySnapshot(lifecycle_.history_guard(), &snapshot, &error)) {
    return {};
  }
  if (pending_merge_preview_) {
    const auto hidden_id = pending_merge_preview_->incoming_version_id;
    snapshot.versions.erase(std::remove_if(snapshot.versions.begin(), snapshot.versions.end(),
                                           [&hidden_id](const EditorHistoryVersion& version) {
                                             return version.version_id == hidden_id;
                                           }),
                            snapshot.versions.end());
  }
  return snapshot;
}

auto EditorSessionService::CancelPendingMergeForNavigation(std::string* error) -> bool {
  if (!pending_merge_preview_) return true;
  if (!dependencies_.history || !lifecycle_.has_history_guard()) {
    if (error) *error = "Cannot discard the pending editor merge without a history guard";
    return false;
  }
  if (!dependencies_.history->CancelMerge(lifecycle_.history_guard(), *pending_merge_preview_,
                                          error)) {
    if (error && error->empty()) *error = "Failed to discard the pending editor merge";
    return false;
  }
  pending_merge_preview_.reset();
  return true;
}

auto EditorSessionService::CreateRootVersion(std::string display_name) -> EditorSessionResult {
  std::string merge_error;
  if (!CancelPendingMergeForNavigation(&merge_error)) {
    return Reject(std::move(merge_error));
  }
  return FinishVersionNavigation(navigation_.RequestCreateRootVersion(std::move(display_name)));
}

auto EditorSessionService::BranchFromCommit(const commit_hash_t& commit_id,
                                            std::string          display_name)
    -> EditorSessionResult {
  std::string merge_error;
  if (!CancelPendingMergeForNavigation(&merge_error)) {
    return Reject(std::move(merge_error));
  }
  return FinishVersionNavigation(
      navigation_.RequestBranchFromCommit(commit_id, std::move(display_name)));
}

auto EditorSessionService::RetrySave() -> EditorSessionResult {
  return FinishVersionNavigation(navigation_.RetrySaveAfterFailure());
}

auto EditorSessionService::DiscardAndContinue() -> EditorSessionResult {
  return FinishVersionNavigation(navigation_.DiscardAndContinueAfterFailure());
}

auto EditorSessionService::CancelPendingNavigation() -> EditorSessionResult {
  navigation_.CancelPendingNavigation();
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::StateChanged;
  result.state    = lifecycle_.state();
  result.identity = lifecycle_.identity();
  result.message  = "Pending navigation cancelled";
  BumpHistoryRevision();
  return Emit(std::move(result));
}

auto EditorSessionService::RenameVersion(const version_ref_id_t& version_id,
                                         std::string display_name) -> EditorSessionResult {
  if (lifecycle_.state() != EditorSessionState::Interactive || !dependencies_.history) {
    return Reject("Version rename requires an interactive session");
  }
  std::string error;
  if (!dependencies_.history->RenameVersion(lifecycle_.history_guard(), version_id,
                                            std::move(display_name), &error)) {
    return Reject(error.empty() ? "Version rename failed" : std::move(error));
  }
  BumpHistoryRevision();
  return StartHistoryCheckpoint("Version renamed", false);
}

auto EditorSessionService::RemoveVersion(const version_ref_id_t& version_id)
    -> EditorSessionResult {
  if (lifecycle_.state() != EditorSessionState::Interactive || !dependencies_.history) {
    return Reject("Version removal requires an interactive session");
  }
  std::string error;
  if (!dependencies_.history->RemoveVersion(lifecycle_.history_guard(), version_id, &error)) {
    return Reject(error.empty() ? "Version removal failed" : std::move(error));
  }
  BumpHistoryRevision();
  return StartHistoryCheckpoint("Version removed", false);
}

auto EditorSessionService::PasteAdjustments(const AdjustmentTransferPackage& package,
                                            std::string                      version_display_name)
    -> EditorSessionResult {
  if (lifecycle_.state() != EditorSessionState::Interactive || !dependencies_.history) {
    return Reject("Paste requires an interactive session");
  }
  AdjustmentPasteResult paste_result;
  std::string           error;
  if (!dependencies_.history->PasteAdjustments(lifecycle_.history_guard(), package,
                                               std::move(version_display_name), &paste_result,
                                               &error)) {
    return Reject(error.empty() ? "Editor Paste failed" : std::move(error));
  }
  BumpHistoryRevision();
  return StartHistoryCheckpoint("Adjustments pasted", true);
}

auto EditorSessionService::BeginMerge(const AdjustmentTransferPackage& package,
                                      AdjustmentMergePreview* preview) -> EditorSessionResult {
  if (lifecycle_.state() != EditorSessionState::Interactive || !dependencies_.history) {
    return Reject("Merge requires an interactive session");
  }
  if (pending_merge_preview_) {
    return Reject("A merge is already awaiting resolution");
  }
  if (preview == nullptr) {
    return Reject("Merge preview storage is required");
  }
  AdjustmentMergePreview next_preview;
  std::string            error;
  if (!dependencies_.history->BeginMerge(lifecycle_.history_guard(), package,
                                         "Incoming Adjustments", &next_preview, &error)) {
    return Reject(error.empty() ? "Editor Merge failed to start" : std::move(error));
  }
  const bool has_conflicts = next_preview.has_conflicts;
  pending_merge_preview_   = std::make_unique<AdjustmentMergePreview>(std::move(next_preview));
  *preview                 = *pending_merge_preview_;
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::Accepted;
  result.state    = lifecycle_.state();
  result.identity = lifecycle_.identity();
  result.message  = has_conflicts ? "Merge requires field resolutions" : "Merge is ready to apply";
  // The pending merge hides its incoming Version from the history projection;
  // publish that visible-set change without waiting for the eventual merge
  // checkpoint.
  BumpHistoryRevision();
  return Emit(std::move(result));
}

auto EditorSessionService::CompleteMerge(const std::vector<AdjustmentMergeResolution>& resolutions)
    -> EditorSessionResult {
  if (lifecycle_.state() != EditorSessionState::Interactive || !dependencies_.history) {
    return Reject("Merge completion requires an interactive session");
  }
  if (!pending_merge_preview_) {
    return Reject("No merge is awaiting resolution");
  }
  AdjustmentMergeResult merge_result;
  std::string           error;
  if (!dependencies_.history->CompleteMerge(lifecycle_.history_guard(), *pending_merge_preview_,
                                            resolutions, &merge_result, &error)) {
    return Reject(error.empty() ? "Merge could not be completed" : std::move(error));
  }
  pending_merge_preview_.reset();
  BumpHistoryRevision();
  return StartHistoryCheckpoint("Adjustments merged", true);
}

auto EditorSessionService::CancelMerge() -> EditorSessionResult {
  if (!dependencies_.history || !lifecycle_.has_history_guard()) {
    return Reject("No active editor merge");
  }
  if (!pending_merge_preview_) {
    return Reject("No merge is awaiting resolution");
  }
  std::string error;
  if (!dependencies_.history->CancelMerge(lifecycle_.history_guard(), *pending_merge_preview_,
                                          &error)) {
    return Reject(error.empty() ? "Merge cancellation failed" : std::move(error));
  }
  pending_merge_preview_.reset();
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::Accepted;
  result.state    = lifecycle_.state();
  result.identity = lifecycle_.identity();
  result.message  = "Merge cancelled";
  BumpHistoryRevision();
  return Emit(std::move(result));
}

auto EditorSessionService::StartHistoryCheckpoint(std::string success_message, bool route_render)
    -> EditorSessionResult {
  if (!dependencies_.history || !lifecycle_.has_history_guard()) {
    return Reject("Editor history is unavailable");
  }
  if (navigation_.has_pending_action() || save_service_.active()) {
    return Reject("Editor save checkpoint is in progress");
  }

  const auto identity = lifecycle_.identity();
  if (dependencies_.journal) {
    std::string finalize_error;
    if (!dependencies_.journal->FinalizeEdit(identity.element_id, identity.session_generation,
                                             &finalize_error)) {
      return Reject(finalize_error.empty() ? "Editor command could not be finalized"
                                           : std::move(finalize_error));
    }
  }
  auto save_lock = save_service_.TryAcquireSaveLock(identity.element_id);
  if (!save_lock.owns_lock()) {
    return Reject("Another editor save checkpoint is in progress");
  }
  std::string capture_error;
  auto        capture =
      dependencies_.history->CaptureSaveCheckpoint(lifecycle_.history_guard(), &capture_error);
  if (!capture || !capture_error.empty()) {
    return Reject(capture_error.empty() ? "Editor history capture failed"
                                        : std::move(capture_error));
  }

  auto                  completed = std::make_shared<std::optional<EditorSessionResult>>();
  SaveCheckpointRequest request;
  request.element_id         = identity.element_id;
  request.session_generation = identity.session_generation;
  request.capture            = std::move(capture);
  if (request.capture->has_journal_range()) {
    request.last_journal_sequence = request.capture->last_journal_sequence;
  }
  request.save_lock = std::move(save_lock);
  lifecycle_.BeginCheckpoint();
  const auto ticket = save_service_.Start(
      std::move(request), [this, completed, success_message = std::move(success_message),
                           route_render](const SaveCheckpointResult& result) mutable {
        EditorSessionResult published;
        published.identity = lifecycle_.identity();
        published.state    = lifecycle_.state();
        published.task_id  = result.task_id;
        if (!result.checkpoint_completed) {
          lifecycle_.KeepCurrentAfterCheckpointFailure(
              result.error.empty() ? "Editor save checkpoint failed" : result.error);
          published.kind    = EditorSessionResultKind::Failed;
          published.state   = lifecycle_.state();
          published.message = lifecycle_.last_error();
          *completed        = published;
          Emit(std::move(published));
          return;
        }
        if (result.last_journal_sequence.has_value()) {
          std::string discard_error;
          (void)dependencies_.history->DiscardMaterializedJournalThrough(
              lifecycle_.history_guard(), *result.last_journal_sequence, &discard_error);
        }
        // The checkpoint materialized the active head to DuckDB without advancing the
        // in-memory ImageEditState.materialized_*. Mirror the durable tuple in memory so
        // a later version/checkout persistence guard accepts it instead of rejecting the
        // durable state as stale.
        if (dependencies_.history != nullptr && lifecycle_.has_history_guard()) {
          std::string sync_error;
          (void)dependencies_.history->SyncMaterializedStateAfterCheckpoint(
              lifecycle_.history_guard(), &sync_error);
        }
        lifecycle_.CompleteCheckpoint();
        if (route_render) {
          EditorRenderAdjustmentSnapshot snapshot;
          std::string                    snapshot_error;
          if (!dependencies_.history->ReadAdjustmentSnapshot(lifecycle_.history_guard(), &snapshot,
                                                             &snapshot_error)) {
            lifecycle_.Fail(snapshot_error.empty() ? "Failed to publish editor history snapshot"
                                                   : snapshot_error);
            published.kind    = EditorSessionResultKind::Failed;
            published.state   = lifecycle_.state();
            published.message = lifecycle_.last_error();
            *completed        = published;
            Emit(std::move(published));
            return;
          }
          edit_.set_adjustment_snapshot(std::move(snapshot));
          lifecycle_.AdvanceRenderGeneration();
          EditorRenderCommand command;
          command.reason     = EditorRenderReason::InitialFrame;
          command.adjustment = edit_.adjustment_snapshot();
          render_.RouteInitialRender(command, lifecycle_.identity());
          published.kind              = EditorSessionResultKind::RenderRouted;
          published.render_request_id = render_.first_frame_request_id();
        } else {
          published.kind = EditorSessionResultKind::Accepted;
        }
        published.state    = lifecycle_.state();
        published.identity = lifecycle_.identity();
        published.message  = success_message;
        *completed         = published;
        Emit(std::move(published));
      });

  if (completed->has_value()) {
    return completed->value();
  }
  if (!ticket.valid()) {
    lifecycle_.KeepCurrentAfterCheckpointFailure("Editor save checkpoint could not start");
    return Reject("Editor save checkpoint could not start");
  }
  EditorSessionResult started;
  started.kind     = EditorSessionResultKind::SaveStarted;
  started.state    = lifecycle_.state();
  started.identity = lifecycle_.identity();
  started.task_id  = ticket.task_id;
  started.message  = "Waiting for editor history checkpoint";
  return Emit(std::move(started));
}

auto EditorSessionService::Switch(sl_element_id_t element_id, image_id_t image_id)
    -> EditorSessionResult {
  std::string merge_error;
  if (!CancelPendingMergeForNavigation(&merge_error)) {
    return Reject(std::move(merge_error));
  }
  const auto outcome = navigation_.RequestOpenOrSwitch(element_id, image_id, true);
  if (outcome.rejected) {
    return Reject(outcome.message);
  }
  if (outcome.failed) {
    return Fail(outcome.message);
  }
  if (outcome.waiting_for_checkpoint) {
    EditorSessionResult waiting;
    waiting.kind     = EditorSessionResultKind::SaveStarted;
    waiting.state    = lifecycle_.state();
    waiting.identity = lifecycle_.identity();
    waiting.task_id  = outcome.ticket.task_id;
    waiting.message  = outcome.message;
    return Emit(std::move(waiting));
  }
  if (outcome.ticket.valid()) {
    EditorSessionResult started;
    started.kind     = EditorSessionResultKind::SaveStarted;
    started.state    = lifecycle_.state();
    started.identity = lifecycle_.identity();
    started.task_id  = outcome.ticket.task_id;
    started.message  = "Save started";
    Emit(std::move(started));
  }
  EditorSessionResult result;
  if (outcome.same_image_noop) {
    result.kind     = EditorSessionResultKind::Accepted;
    result.state    = lifecycle_.state();
    result.identity = lifecycle_.identity();
    result.message  = outcome.message;
    return Emit(std::move(result));
  }
  if (lifecycle_.state() == EditorSessionState::Failed) {
    result.kind    = EditorSessionResultKind::Failed;
    result.message = lifecycle_.last_error();
  } else if (lifecycle_.state() == EditorSessionState::Loading ||
             lifecycle_.state() == EditorSessionState::Acquiring ||
             lifecycle_.state() == EditorSessionState::Switching) {
    if (render_.first_frame_request_id() == 0 && render_.PresentationTargetReady()) {
      result.kind    = EditorSessionResultKind::Failed;
      result.message = "First frame could not be scheduled";
      lifecycle_.Fail(result.message);
    } else if (render_.first_frame_request_id() == 0) {
      result.kind = EditorSessionResultKind::StateChanged;
    } else {
      result.kind              = EditorSessionResultKind::RenderRouted;
      result.render_request_id = render_.first_frame_request_id();
    }
  } else if (lifecycle_.state() == EditorSessionState::Interactive) {
    result.kind = EditorSessionResultKind::Accepted;
  } else if (lifecycle_.state() == EditorSessionState::NoImage) {
    result.kind = EditorSessionResultKind::StateChanged;
  } else {
    result.kind = EditorSessionResultKind::Accepted;
  }
  result.state    = lifecycle_.state();
  result.identity = lifecycle_.identity();
  result.message  = outcome.message;
  if (outcome.ticket.valid()) {
    EditorSessionResult finished;
    finished.kind     = EditorSessionResultKind::SaveFinished;
    finished.state    = lifecycle_.state();
    finished.identity = lifecycle_.identity();
    finished.task_id  = outcome.ticket.task_id;
    finished.message  = "Editor session materialized";
    Emit(std::move(finished));
  }
  BumpHistoryRevision();
  return Emit(std::move(result));
}

auto EditorSessionService::Close(bool persist_changes) -> EditorSessionResult {
  std::string merge_error;
  if (!CancelPendingMergeForNavigation(&merge_error)) {
    return Reject(std::move(merge_error));
  }
  const auto outcome = navigation_.RequestClose(persist_changes);
  if (outcome.rejected) {
    return Reject(outcome.message);
  }
  if (outcome.failed) {
    return Fail(outcome.message);
  }
  if (outcome.waiting_for_checkpoint) {
    EditorSessionResult waiting;
    waiting.kind     = EditorSessionResultKind::SaveStarted;
    waiting.state    = lifecycle_.state();
    waiting.identity = lifecycle_.identity();
    waiting.task_id  = outcome.ticket.task_id;
    waiting.message  = outcome.message;
    return Emit(std::move(waiting));
  }
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::StateChanged;
  result.state    = lifecycle_.state();
  result.identity = lifecycle_.identity();
  result.message  = outcome.message;
  BumpHistoryRevision();
  return Emit(std::move(result));
}

auto EditorSessionService::Patch(EditorAdjustmentPatch patch) -> EditorSessionResult {
  if (lifecycle_.state() != EditorSessionState::Interactive) {
    return Reject("Patch requires an interactive session");
  }
  if (!lifecycle_.has_image()) {
    return Reject("Patch requires an open image");
  }
  const auto guard   = lifecycle_.history_guard();
  const auto ident   = lifecycle_.identity();
  const auto outcome = edit_.HandlePatch(std::move(patch), false, guard, ident);
  if (outcome.kind == EditorEditOutcome::Kind::Rejected) {
    return Reject(outcome.message);
  }
  lifecycle_.AdvanceRenderGeneration();
  const auto route_identity = lifecycle_.identity();
  render_.RouteInitialRender(outcome.render_command, route_identity);
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::RenderRouted;
  result.state    = lifecycle_.state();
  result.identity = route_identity;
  result.message  = outcome.message;
  return Emit(std::move(result));
}

auto EditorSessionService::CommitAdjustment(EditorAdjustmentPatch patch) -> EditorSessionResult {
  if (lifecycle_.state() != EditorSessionState::Interactive) {
    return Reject("Patch requires an interactive session");
  }
  if (!lifecycle_.has_image()) {
    return Reject("Patch requires an open image");
  }
  const auto guard   = lifecycle_.history_guard();
  const auto ident   = lifecycle_.identity();
  const auto outcome = edit_.HandlePatch(std::move(patch), true, guard, ident);
  if (outcome.kind == EditorEditOutcome::Kind::Rejected) {
    return Reject(outcome.message);
  }
  lifecycle_.AdvanceRenderGeneration();
  const auto route_identity = lifecycle_.identity();
  render_.RouteInitialRender(outcome.render_command, route_identity);
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::RenderRouted;
  result.state    = lifecycle_.state();
  result.identity = route_identity;
  result.message  = outcome.message;
  BumpHistoryRevision();
  return Emit(std::move(result));
}

auto EditorSessionService::Patch(std::string patch_key) -> EditorSessionResult {
  EditorAdjustmentPatch patch;
  patch.field_key = std::move(patch_key);
  return Patch(std::move(patch));
}

auto EditorSessionService::CommitAdjustment(std::string patch_key) -> EditorSessionResult {
  EditorAdjustmentPatch patch;
  patch.field_key = std::move(patch_key);
  patch.settled   = true;
  return CommitAdjustment(std::move(patch));
}

auto EditorSessionService::Undo() -> EditorSessionResult {
  if (lifecycle_.state() != EditorSessionState::Interactive) {
    return Reject("Undo requires interactive state");
  }
  const auto guard   = lifecycle_.history_guard();
  const auto ident   = lifecycle_.identity();
  const auto outcome = edit_.HandleUndoRedo(true, guard, ident);
  if (outcome.kind == EditorEditOutcome::Kind::Rejected) {
    return Reject(outcome.message);
  }
  if (outcome.kind == EditorEditOutcome::Kind::Failed) {
    return Reject(outcome.message);
  }
  lifecycle_.AdvanceRenderGeneration();
  const auto undo_identity = lifecycle_.identity();
  render_.RouteInitialRender(outcome.render_command, undo_identity);
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::Accepted;
  result.state    = lifecycle_.state();
  result.identity = undo_identity;
  result.message  = outcome.message;
  BumpHistoryRevision();
  return Emit(std::move(result));
}

auto EditorSessionService::Redo() -> EditorSessionResult {
  if (lifecycle_.state() != EditorSessionState::Interactive) {
    return Reject("Redo requires interactive state");
  }
  const auto guard   = lifecycle_.history_guard();
  const auto ident   = lifecycle_.identity();
  const auto outcome = edit_.HandleUndoRedo(false, guard, ident);
  if (outcome.kind == EditorEditOutcome::Kind::Rejected) {
    return Reject(outcome.message);
  }
  if (outcome.kind == EditorEditOutcome::Kind::Failed) {
    return Reject(outcome.message);
  }
  lifecycle_.AdvanceRenderGeneration();
  const auto redo_identity = lifecycle_.identity();
  render_.RouteInitialRender(outcome.render_command, redo_identity);
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::Accepted;
  result.state    = lifecycle_.state();
  result.identity = redo_identity;
  result.message  = outcome.message;
  BumpHistoryRevision();
  return Emit(std::move(result));
}

auto EditorSessionService::MoveHeadToCommit(const commit_hash_t& commit_id)
    -> EditorSessionResult {
  if (lifecycle_.state() != EditorSessionState::Interactive) {
    return Reject("Editor head move requires interactive state");
  }
  const auto guard   = lifecycle_.history_guard();
  const auto ident   = lifecycle_.identity();
  const auto outcome = edit_.HandleMoveHeadToCommit(commit_id, guard, ident);
  if (outcome.kind == EditorEditOutcome::Kind::Rejected) {
    return Reject(outcome.message);
  }
  if (outcome.kind == EditorEditOutcome::Kind::Failed) {
    return Reject(outcome.message);
  }
  lifecycle_.AdvanceRenderGeneration();
  const auto move_identity = lifecycle_.identity();
  render_.RouteInitialRender(outcome.render_command, move_identity);
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::Accepted;
  result.state    = lifecycle_.state();
  result.identity = move_identity;
  result.message  = outcome.message;
  BumpHistoryRevision();
  return Emit(std::move(result));
}

auto EditorSessionService::Discard() -> EditorSessionResult {
  const auto state = lifecycle_.state();
  if (state != EditorSessionState::Interactive && state != EditorSessionState::Failed) {
    return Reject("Discard requires an image with an active history session");
  }
  std::string merge_error;
  if (!CancelPendingMergeForNavigation(&merge_error)) {
    return Reject(std::move(merge_error));
  }
  const auto guard   = lifecycle_.history_guard();
  const auto ident   = lifecycle_.identity();
  const auto outcome = edit_.HandleDiscard(guard, ident, state);
  if (outcome.kind == EditorEditOutcome::Kind::Rejected) {
    return Reject(outcome.message);
  }
  if (outcome.kind == EditorEditOutcome::Kind::Failed) {
    return Reject(outcome.message);
  }
  lifecycle_.AdvanceRenderGeneration();
  if (state == EditorSessionState::Failed) {
    lifecycle_.BeginRetryFromDiscard();
  }
  const auto discard_identity = lifecycle_.identity();
  render_.RouteInitialRender(outcome.render_command, discard_identity);
  EditorSessionResult result;
  result.kind     = (state == EditorSessionState::Failed) ? EditorSessionResultKind::StateChanged
                                                          : EditorSessionResultKind::Accepted;
  result.state    = lifecycle_.state();
  result.identity = lifecycle_.identity();
  result.message  = outcome.message;
  BumpHistoryRevision();
  return Emit(std::move(result));
}

auto EditorSessionService::Shutdown() -> EditorSessionResult {
  if (lifecycle_.state() == EditorSessionState::ShuttingDown) {
    return Reject("Already shutting down");
  }
  std::string merge_error;
  if (!CancelPendingMergeForNavigation(&merge_error)) {
    return Reject(std::move(merge_error));
  }
  // Cancel outstanding save work and wait for callback drain. CancelAndWait
  // publishes one terminal cancellation per in-flight checkpoint; navigation
  // keeps image A on that failure path and clears the pending action.
  save_service_.CancelAndWait();
  navigation_.ClearPendingAction();
  lifecycle_.BeginShutdown();
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::StateChanged;
  result.state    = lifecycle_.state();
  result.identity = lifecycle_.identity();
  result.message  = "Shutting down";
  BumpHistoryRevision();
  return Emit(std::move(result));
}

auto EditorSessionService::RequestViewChange(EditorRenderReason                  reason,
                                             std::optional<ViewportRenderRegion> region)
    -> EditorSessionResult {
  EditorRenderCommand command;
  command.reason      = reason;
  command.adjustment  = edit_.adjustment_snapshot();
  command.view_region = std::move(region);
  // Advance the appropriate generation before routing to the render
  // controller. The render controller no longer mutates lifecycle.
  if (reason == EditorRenderReason::CropRotate) {
    lifecycle_.AdvanceRenderGeneration();
  } else {
    lifecycle_.AdvanceViewGeneration();
  }
  const auto          view_identity = lifecycle_.identity();
  const auto          view_state    = lifecycle_.state();
  const auto          event         = render_.RouteViewChange(command, view_identity, view_state);
  EditorSessionResult result;
  result.state    = event.state;
  result.identity = event.identity;
  switch (event.kind) {
    case EditorRenderEventKind::RenderRouted:
      result.kind              = EditorSessionResultKind::RenderRouted;
      result.render_request_id = event.request_id;
      result.message           = event.message;
      break;
    case EditorRenderEventKind::RenderReused:
      result.kind    = EditorSessionResultKind::Accepted;
      result.message = event.message;
      break;
    default:
      result.kind    = EditorSessionResultKind::Rejected;
      result.message = event.message;
      break;
  }
  return Emit(std::move(result));
}

void EditorSessionService::NotifyImageAcquired(std::uint64_t session_generation, bool success,
                                               std::string message) {
  const auto identity = lifecycle_.identity();
  if (identity.session_generation != session_generation) {
    return;
  }
  const auto state = lifecycle_.state();
  if (state != EditorSessionState::Loading && state != EditorSessionState::Acquiring &&
      state != EditorSessionState::Switching) {
    return;
  }
  if (!success) {
    lifecycle_.ReleaseGuards();
    render_.ResetForNewImage();
    lifecycle_.Fail(message.empty() ? "Image acquisition failed" : std::move(message));
    EditorSessionResult result;
    result.kind     = EditorSessionResultKind::Failed;
    result.state    = lifecycle_.state();
    result.identity = lifecycle_.identity();
    result.message  = lifecycle_.last_error();
    Emit(std::move(result));
    return;
  }
  render_.MarkImageAcquired();
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::Accepted;
  result.state    = lifecycle_.state();
  result.identity = identity;
  result.message = message.empty() ? "Image acquired; waiting for first frame" : std::move(message);
  Emit(std::move(result));
}

}  // namespace alcedo
