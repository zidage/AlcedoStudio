//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_session_service.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <utility>

namespace alcedo {

struct EditorSessionService::AsyncCallbackGate {
  auto Enter() -> bool {
    std::scoped_lock lock(mutex);
    if (stopping) {
      return false;
    }
    ++active_callbacks;
    return true;
  }

  void Leave() {
    std::scoped_lock lock(mutex);
    if (active_callbacks > 0) {
      --active_callbacks;
    }
    if (stopping && active_callbacks == 0) {
      condition.notify_all();
    }
  }

  void StopAndWait() {
    std::unique_lock lock(mutex);
    stopping = true;
    condition.wait(lock, [this] { return active_callbacks == 0; });
  }

  std::mutex              mutex;
  std::condition_variable condition;
  std::size_t             active_callbacks = 0;
  bool                    stopping         = false;
};

EditorSessionService::EditorSessionService(Dependencies dependencies)
    : dependencies_(std::move(dependencies)),
      callback_gate_(std::make_shared<AsyncCallbackGate>()) {}

EditorSessionService::~EditorSessionService() {
  if (callback_gate_) {
    callback_gate_->StopAndWait();
  }
}

void EditorSessionService::SetResultObserver(ResultObserver observer) {
  std::scoped_lock lock(mutex_);
  observer_ = std::move(observer);
}

void EditorSessionService::SetChangeNotifier(ChangeNotifier notifier) {
  std::scoped_lock lock(mutex_);
  change_notifier_ = std::move(notifier);
}

void EditorSessionService::SetPresentationSinkId(PresentationSinkId sink_id) {
  std::scoped_lock lock(mutex_);
  presentation_sink_id_ = sink_id;
  if (sink_id == 0) {
    presentation_width_  = 0;
    presentation_height_ = 0;
    return;
  }
  RoutePendingInitialRender();
}

void EditorSessionService::SetPresentationSize(int width, int height) {
  std::scoped_lock lock(mutex_);
  presentation_width_  = std::max(0, width);
  presentation_height_ = std::max(0, height);
  RoutePendingInitialRender();
}

auto EditorSessionService::Emit(EditorSessionResult result) -> EditorSessionResult {
  results_.push_back(result);
  if (observer_) {
    observer_(results_.back());
  }
  NotifyChange();
  return result;
}

auto EditorSessionService::Reject(std::string message) -> EditorSessionResult {
  last_error_ = message;
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::Rejected;
  result.state    = state_;
  result.identity = identity_;
  result.message  = std::move(message);
  return Emit(std::move(result));
}

auto EditorSessionService::TransitionTo(EditorSessionState next, EditorSessionResultKind kind,
                                        std::string message) -> EditorSessionResult {
  state_ = next;
  if (kind == EditorSessionResultKind::Failed) {
    last_error_ = message;
  } else if (kind != EditorSessionResultKind::Rejected) {
    last_error_.clear();
  }
  EditorSessionResult result;
  result.kind     = kind;
  result.state    = state_;
  result.identity = identity_;
  result.message  = std::move(message);
  return Emit(std::move(result));
}

void EditorSessionService::ReleaseGuards() {
  if (dependencies_.history && history_guard_.valid) {
    dependencies_.history->Release(history_guard_);
  }
  history_guard_ = {};
  if (dependencies_.pipeline && pipeline_guard_.valid) {
    dependencies_.pipeline->Release(pipeline_guard_);
  }
  pipeline_guard_ = {};
}

auto EditorSessionService::AcquireGuards(sl_element_id_t element_id, std::string* error) -> bool {
  if (!dependencies_.pipeline || !dependencies_.history) {
    if (error) {
      *error = "Pipeline or history port is missing";
    }
    return false;
  }
  pipeline_guard_ = dependencies_.pipeline->Acquire(element_id, error);
  if (!pipeline_guard_.valid) {
    return false;
  }
  history_guard_ = dependencies_.history->Acquire(element_id, error);
  if (!history_guard_.valid) {
    dependencies_.pipeline->Release(pipeline_guard_);
    pipeline_guard_ = {};
    return false;
  }
  return true;
}

auto EditorSessionService::MakeRenderIntent(EditorRenderReason reason) const
    -> std::optional<EditorRenderIntent> {
  std::scoped_lock lock(mutex_);
  if (!has_image() && state_ != EditorSessionState::Loading &&
      state_ != EditorSessionState::Acquiring && state_ != EditorSessionState::Switching) {
    return std::nullopt;
  }
  EditorRenderIntent intent;
  intent.element_id           = identity_.element_id;
  intent.image_id             = identity_.image_id;
  intent.session_generation   = identity_.session_generation;
  intent.render_generation    = identity_.render_generation;
  intent.view_generation      = identity_.view_generation;
  intent.reason               = reason;
  intent.quality              = DefaultQualityForReason(reason);
  intent.priority             = DefaultPriorityForReason(reason);
  intent.frame_role           = FrameRoleForQuality(intent.quality);
  intent.replacement_key      = DefaultReplacementKey(intent.quality);
  intent.adjustment           = adjustment_snapshot_;
  intent.requested_width      = presentation_width_;
  intent.requested_height     = presentation_height_;
  intent.presentation_sink_id = presentation_sink_id_;
  intent.cancellation         = std::make_shared<EditorRenderCancellationToken>();
  FillRenderIntentDefaults(intent);
  return intent;
}

auto EditorSessionService::render_diagnostics() const -> EditorRenderCoordinatorDiagnostics {
  if (!dependencies_.render) {
    return {};
  }
  return dependencies_.render->diagnostics();
}

auto EditorSessionService::first_frame_time_ms() const -> double {
  std::scoped_lock lock(mutex_);
  return first_frame_time_ms_;
}

auto EditorSessionService::RouteInitialRender(EditorRenderReason reason,
                                              EditorRenderSupersessionPolicy policy)
    -> std::uint64_t {
  if (!PresentationTargetReady()) {
    pending_initial_reason_ = reason;
    return 0;
  }
  if (!dependencies_.render) {
    return 0;
  }
  auto intent = MakeRenderIntent(reason);
  if (!intent) {
    return 0;
  }
  dependencies_.render->SetActiveGenerations(
      identity_.session_generation, identity_.render_generation, identity_.view_generation,
      policy);
  const EditorRenderResult routed = dependencies_.render->Submit(*intent);
  if (routed.kind == EditorRenderResultKind::RequestAccepted) {
    if (reason == EditorRenderReason::InitialFrame || reason == EditorRenderReason::ImageSwitch ||
        reason == EditorRenderReason::Retry) {
      first_frame_request_id_  = routed.request_id;
      first_frame_completed_   = false;
      first_frame_submitted_   = false;
      first_frame_presented_   = false;
      quality_base_routed_     = false;
      quality_base_request_id_ = 0;
      // Phase 5E: start first-frame latency from the moment the InteractivePrimary
      // request is accepted by the sole coordinator.
      first_frame_route_time_  = std::chrono::steady_clock::now();
      first_frame_time_ms_     = -1.0;
    }
    pending_initial_reason_.reset();
    EditorSessionResult session_result;
    session_result.kind              = EditorSessionResultKind::RenderRouted;
    session_result.state             = state_;
    session_result.identity          = identity_;
    session_result.render_request_id = routed.request_id;
    Emit(session_result);
    return routed.request_id;
  }
  return 0;
}

auto EditorSessionService::RouteQualityBaseFollowUp() -> std::uint64_t {
  if (quality_base_routed_ || !PresentationTargetReady() || !dependencies_.render || !has_image()) {
    return 0;
  }
  auto intent = MakeRenderIntent(EditorRenderReason::SettledAdjustment);
  if (!intent) {
    return 0;
  }
  // Force QualityBase regardless of MakeRenderIntent reason defaults.
  intent->quality         = EditorRenderQuality::Quality;
  intent->frame_role      = FrameRole::QualityBase;
  intent->priority        = EditorRenderPriority::Normal;
  intent->replacement_key = DefaultReplacementKey(EditorRenderQuality::Quality);
  FillRenderIntentDefaults(*intent);

  dependencies_.render->SetActiveGenerations(
      identity_.session_generation, identity_.render_generation, identity_.view_generation);
  const EditorRenderResult routed = dependencies_.render->Submit(*intent);
  if (routed.kind != EditorRenderResultKind::RequestAccepted) {
    return 0;
  }
  quality_base_routed_     = true;
  quality_base_request_id_ = routed.request_id;
  EditorSessionResult session_result;
  session_result.kind              = EditorSessionResultKind::RenderRouted;
  session_result.state             = state_;
  session_result.identity          = identity_;
  session_result.render_request_id = routed.request_id;
  session_result.message           = "QualityBase follow-up after first frame";
  Emit(session_result);
  return routed.request_id;
}

void EditorSessionService::MarkImageAcquiredAfterGuards() {
  // Pipeline/history guards mean the image is ready to render. Stay in Loading
  // until the matching InteractivePrimary frame is presented.
  image_acquired_ = true;
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::Accepted;
  result.state    = state_;
  result.identity = identity_;
  result.message  = "Image acquired; waiting for first frame";
  Emit(std::move(result));
}

auto EditorSessionService::PresentationTargetReady() const -> bool {
  return presentation_sink_id_ != 0 && presentation_width_ > 0 && presentation_height_ > 0;
}

void EditorSessionService::RoutePendingInitialRender() {
  if (!pending_initial_reason_.has_value() || !PresentationTargetReady() || !has_image()) {
    return;
  }
  const EditorRenderReason reason = *pending_initial_reason_;
  if (RouteInitialRender(reason) == 0 && pending_initial_reason_.has_value()) {
    TransitionTo(EditorSessionState::Failed, EditorSessionResultKind::Failed,
                 "First frame could not be scheduled");
  }
}

auto EditorSessionService::BeginSaveForSession(std::uint64_t   session_generation,
                                               sl_element_id_t element_id, std::string* error)
    -> bool {
  std::uint64_t task_id = 0;
  if (dependencies_.tasks) {
    task_id = dependencies_.tasks->BeginTask("editor_save", element_id);
    if (task_id == 0) {
      if (error) {
        *error = "Failed to start editor save task";
      }
      return false;
    }
  }
  pending_saves_.push_back(PendingSave{session_generation, element_id, task_id});
  state_ = EditorSessionState::Saving;
  EditorSessionResult started;
  started.kind     = EditorSessionResultKind::SaveStarted;
  started.state    = state_;
  started.identity = identity_;
  started.task_id  = task_id;
  started.message  = "Save started";
  Emit(std::move(started));

  struct StartObservation {
    std::atomic<bool> completed{false};
    std::atomic<bool> commit_succeeded{true};
    std::string       error;
  };
  const auto observation = std::make_shared<StartObservation>();
  const auto gate        = callback_gate_;
  const auto on_commit   = [this, gate, observation,
                          session_generation](EditorJournalCommitOutcome outcome) {
    observation->error = outcome.error;
    observation->commit_succeeded.store(outcome.accepted && outcome.durable,
                                          std::memory_order_release);
    observation->completed.store(true, std::memory_order_release);
    if (!gate || !gate->Enter()) {
      return;
    }
    HandleJournalCommit(session_generation, std::move(outcome));
    gate->Leave();
  };

  bool started_async = true;
  if (dependencies_.journal) {
    started_async =
        dependencies_.journal->CommitJournalAsync(element_id, session_generation, on_commit);
  } else {
    on_commit(EditorJournalCommitOutcome{true, true, false, 0, 0, {}});
  }
  if (!started_async) {
    if (!observation->completed.load(std::memory_order_acquire)) {
      auto it = std::find_if(pending_saves_.begin(), pending_saves_.end(),
                             [session_generation](const PendingSave& save) {
                               return save.session_generation == session_generation;
                             });
      if (it != pending_saves_.end()) {
        const auto failed_task_id = it->task_id;
        pending_saves_.erase(it);
        if (dependencies_.tasks && failed_task_id != 0) {
          dependencies_.tasks->EndTask(failed_task_id, false, "Journal commit could not start");
        }
      }
    }
    if (error) {
      *error = observation->error.empty() ? "Journal commit could not start" : observation->error;
    }
    return false;
  }

  // Legacy ports complete synchronously. Preserve their historical failure
  // behavior so a failed barrier keeps the old image active; true async ports
  // return before this observation is completed and let B proceed.
  if (observation->completed.load(std::memory_order_acquire) &&
      !observation->commit_succeeded.load(std::memory_order_acquire)) {
    if (error) {
      *error = observation->error.empty() ? "Journal commit failed" : observation->error;
    }
    return false;
  }
  return true;
}

void EditorSessionService::HandleJournalCommit(std::uint64_t              session_generation,
                                               EditorJournalCommitOutcome outcome) {
  std::scoped_lock lock(mutex_);
  auto             pending = std::find_if(pending_saves_.begin(), pending_saves_.end(),
                                          [session_generation](const PendingSave& save) {
                                return save.session_generation == session_generation;
                              });
  if (pending == pending_saves_.end()) {
    return;
  }
  if (!outcome.accepted || !outcome.durable) {
    NotifySaveFinished(session_generation, false,
                       outcome.error.empty() ? "Journal commit failed" : outcome.error);
    return;
  }

  if (!dependencies_.journal) {
    NotifySaveFinished(session_generation, true, "Journal commit complete");
    return;
  }

  const auto gate    = callback_gate_;
  const bool started = dependencies_.journal->MaterializeAsync(
      pending->element_id, session_generation,
      [this, gate, session_generation](EditorMaterializeOutcome materialized) mutable {
        if (!gate || !gate->Enter()) {
          return;
        }
        HandleMaterialization(session_generation, std::move(materialized));
        gate->Leave();
      });
  if (!started) {
    NotifySaveFinished(session_generation, false, "Materialization could not start");
  }
}

void EditorSessionService::HandleMaterialization(std::uint64_t            session_generation,
                                                 EditorMaterializeOutcome outcome) {
  std::scoped_lock lock(mutex_);
  NotifySaveFinished(
      session_generation, outcome.accepted && outcome.materialized,
      outcome.error.empty()
          ? (outcome.materialized ? "Editor session materialized" : "Editor materialization failed")
          : outcome.error);
}

auto EditorSessionService::SealCurrentSession(bool persist_changes, bool start_background_save,
                                              std::string* error) -> bool {
  if (identity_.element_id == 0 || identity_.image_id == 0) {
    return true;
  }

  if (persist_changes) {
    if (dependencies_.journal && !dependencies_.journal->FinalizeEdit(
                                     identity_.element_id, identity_.session_generation, error)) {
      return false;
    }
    if (start_background_save &&
        !BeginSaveForSession(identity_.session_generation, identity_.element_id, error)) {
      return false;
    }
  } else if (dependencies_.journal &&
             !dependencies_.journal->DiscardUnflushed(identity_.element_id, error)) {
    return false;
  }

  if (dependencies_.render && identity_.session_generation != 0) {
    // Teardown order (Phase 5E): cancel obsolete work and wait until session
    // workers leave IFrameSink before the presentation consumer is destroyed.
    dependencies_.render->CancelSessionAndWait(identity_.session_generation);
  }
  ReleaseGuards();
  return true;
}

void EditorSessionService::ResetActiveImageState() {
  identity_.element_id        = 0;
  identity_.image_id          = 0;
  identity_.render_generation = 0;
  identity_.view_generation   = 0;
  adjustment_snapshot_        = {};
  first_frame_request_id_     = 0;
  quality_base_request_id_    = 0;
  image_acquired_             = false;
  first_frame_completed_      = false;
  first_frame_submitted_      = false;
  first_frame_presented_      = false;
  quality_base_routed_        = false;
  pending_initial_reason_.reset();
  first_frame_route_time_.reset();
  first_frame_time_ms_ = -1.0;
}

auto EditorSessionService::HandleOpenOrSwitch(const EditorSessionIntent& intent, bool is_switch)
    -> EditorSessionResult {
  if (state_ == EditorSessionState::ShuttingDown) {
    return Reject("Cannot open while shutting down");
  }

  const bool open_empty = intent.element_id == 0 || intent.image_id == 0;
  if (open_empty) {
    return HandleClose(/*persist_changes=*/true);
  }

  // Same image already open: no-op. Avoids leaking guards or orphaning renders.
  if (identity_.element_id == intent.element_id && identity_.image_id == intent.image_id &&
      (state_ == EditorSessionState::Loading || state_ == EditorSessionState::Interactive ||
       state_ == EditorSessionState::Acquiring || state_ == EditorSessionState::Switching ||
       state_ == EditorSessionState::Saving)) {
    EditorSessionResult result;
    result.kind     = EditorSessionResultKind::Accepted;
    result.state    = state_;
    result.identity = identity_;
    result.message  = "Image already open";
    return Emit(std::move(result));
  }

  // Seal the prior image before acquiring the next one. Workspace routing calls
  // Switch directly, so this is the single A→B lifecycle path.
  if (identity_.session_generation != 0 && (identity_.element_id != 0 || identity_.image_id != 0)) {
    std::string seal_error;
    if (!SealCurrentSession(/*persist_changes=*/true, /*start_background_save=*/true,
                            &seal_error)) {
      return TransitionTo(EditorSessionState::Failed, EditorSessionResultKind::Failed,
                          seal_error.empty() ? "Failed to save current image" : seal_error);
    }
  }

  ++identity_.session_generation;
  identity_.element_id        = intent.element_id;
  identity_.image_id          = intent.image_id;
  identity_.render_generation = identity_.session_generation;
  identity_.view_generation   = 1;
  image_acquired_             = false;
  first_frame_request_id_     = 0;
  quality_base_request_id_    = 0;
  first_frame_completed_      = false;
  first_frame_submitted_      = false;
  first_frame_presented_      = false;
  quality_base_routed_        = false;
  first_frame_route_time_.reset();
  first_frame_time_ms_ = -1.0;
  // Adjustment state is image-scoped. An empty snapshot means a clean image;
  // never inherit the previous image's params.
  adjustment_snapshot_ = intent.adjustment;

  const EditorSessionState acquire_state =
      is_switch ? EditorSessionState::Switching : EditorSessionState::Acquiring;
  TransitionTo(acquire_state, EditorSessionResultKind::StateChanged,
               is_switch ? "Switching image" : "Acquiring image");

  // Phase 5H-Fix: run journal recovery + materialization so the editor starts
  // from the durable DuckDB state before services cache pipeline/history guards.
  // The journal port emits a diagnostic bundle on failure.
  if (dependencies_.journal) {
    std::string recover_error;
    const auto  recovered = dependencies_.journal->RecoverAndMaterialize(
        intent.element_id, identity_.session_generation, &recover_error);
    if (!recovered.accepted) {
      identity_.element_id = 0;
      identity_.image_id   = 0;
      return TransitionTo(EditorSessionState::Failed, EditorSessionResultKind::Failed,
                          recover_error.empty() ? "Editor journal recovery failed" : recover_error);
    }
  }

  std::string error;
  if (!AcquireGuards(intent.element_id, &error)) {
    identity_.element_id = 0;
    identity_.image_id   = 0;
    return TransitionTo(EditorSessionState::Failed, EditorSessionResultKind::Failed,
                        error.empty() ? "Failed to acquire pipeline/history guards" : error);
  }

  TransitionTo(EditorSessionState::Loading, EditorSessionResultKind::StateChanged, "Loading image");
  // Guards succeeded: the image is ready to render. Interactive still waits for
  // the first compatible frame (Phase 5B open path).
  MarkImageAcquiredAfterGuards();
  const auto request_id = RouteInitialRender(is_switch ? EditorRenderReason::ImageSwitch
                                                       : EditorRenderReason::InitialFrame);
  if (request_id == 0 && PresentationTargetReady()) {
    return TransitionTo(EditorSessionState::Failed, EditorSessionResultKind::Failed,
                        "First frame could not be scheduled");
  }
  return results_.back();
}

auto EditorSessionService::HandleClose(bool persist_changes) -> EditorSessionResult {
  if (state_ == EditorSessionState::ShuttingDown) {
    return Reject("Cannot close while shutting down");
  }

  std::string error;
  if (!SealCurrentSession(persist_changes, /*start_background_save=*/persist_changes, &error)) {
    return TransitionTo(EditorSessionState::Failed, EditorSessionResultKind::Failed,
                        error.empty() ? "Failed to close editor session" : error);
  }
  ResetActiveImageState();
  return TransitionTo(EditorSessionState::NoImage, EditorSessionResultKind::StateChanged,
                      persist_changes ? "Editor session closed" : "Editor changes discarded");
}

auto EditorSessionService::HandlePatch(const EditorSessionIntent& intent, bool settled)
    -> EditorSessionResult {
  if (state_ != EditorSessionState::Interactive) {
    return Reject("Patch requires an interactive session");
  }
  if (!has_image()) {
    return Reject("Patch requires an open image");
  }

  EditorAdjustmentPatch patch = intent.patch;
  patch.settled               = settled;
  if (!intent.adjustment.params_json.empty() || !intent.adjustment.patches.empty() ||
      !intent.adjustment.fingerprint.empty()) {
    adjustment_snapshot_ = intent.adjustment;
  } else if (!patch.field_key.empty()) {
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
  }

  ++identity_.render_generation;
  const auto reason =
      settled ? EditorRenderReason::SettledAdjustment : EditorRenderReason::InteractiveAdjustment;
  const auto request_id = RouteInitialRender(
      reason, EditorRenderSupersessionPolicy::PreserveInflightFullFrame);
  if (request_id == 0) {
    return TransitionTo(EditorSessionState::Failed, EditorSessionResultKind::Failed,
                        "Adjustment render could not be scheduled");
  }
  return results_.back();
}

auto EditorSessionService::HandleUndoRedo(bool undo) -> EditorSessionResult {
  if (state_ != EditorSessionState::Interactive) {
    return Reject(undo ? "Undo requires interactive state" : "Redo requires interactive state");
  }
  if (!dependencies_.history || !history_guard_.valid) {
    return Reject("History port unavailable");
  }
  std::string error;
  const bool  ok = undo ? dependencies_.history->Undo(history_guard_, &error)
                        : dependencies_.history->Redo(history_guard_, &error);
  if (!ok) {
    return TransitionTo(EditorSessionState::Failed, EditorSessionResultKind::Failed,
                        error.empty() ? (undo ? "Undo failed" : "Redo failed") : error);
  }
  EditorRenderAdjustmentSnapshot snapshot;
  if (!dependencies_.history->ReadAdjustmentSnapshot(history_guard_, &snapshot, &error)) {
    return TransitionTo(EditorSessionState::Failed, EditorSessionResultKind::Failed,
                        error.empty() ? "Failed to read history adjustment state" : error);
  }
  adjustment_snapshot_ = std::move(snapshot);
  ++identity_.render_generation;
  if (RouteInitialRender(EditorRenderReason::UndoRedo) == 0) {
    return TransitionTo(EditorSessionState::Failed, EditorSessionResultKind::Failed,
                        "History render could not be scheduled");
  }
  return TransitionTo(EditorSessionState::Interactive, EditorSessionResultKind::Accepted,
                      undo ? "Undo applied" : "Redo applied");
}

auto EditorSessionService::HandleDiscard() -> EditorSessionResult {
  if ((state_ != EditorSessionState::Interactive && state_ != EditorSessionState::Failed) ||
      identity_.element_id == 0 || identity_.image_id == 0 || !history_guard_.valid) {
    return Reject("Discard requires an image with an active history session");
  }
  if (dependencies_.journal) {
    std::string error;
    if (!dependencies_.journal->DiscardUnflushed(identity_.element_id, &error)) {
      return TransitionTo(EditorSessionState::Failed, EditorSessionResultKind::Failed,
                          error.empty() ? "Discard failed" : error);
    }
  }
  std::string                    error;
  EditorRenderAdjustmentSnapshot snapshot;
  if (!dependencies_.history ||
      !dependencies_.history->ReadAdjustmentSnapshot(history_guard_, &snapshot, &error)) {
    return TransitionTo(EditorSessionState::Failed, EditorSessionResultKind::Failed,
                        error.empty() ? "Failed to restore discarded adjustment state" : error);
  }
  adjustment_snapshot_ = std::move(snapshot);
  ++identity_.render_generation;

  if (state_ == EditorSessionState::Failed) {
    TransitionTo(EditorSessionState::Loading, EditorSessionResultKind::StateChanged,
                 "Retrying after discard");
    const auto request_id = RouteInitialRender(EditorRenderReason::Retry);
    if (request_id == 0 && PresentationTargetReady()) {
      return TransitionTo(EditorSessionState::Failed, EditorSessionResultKind::Failed,
                          "Discard retry render could not be scheduled");
    }
    return results_.back();
  }

  if (RouteInitialRender(EditorRenderReason::SettledAdjustment) == 0) {
    return TransitionTo(EditorSessionState::Failed, EditorSessionResultKind::Failed,
                        "Discard render could not be scheduled");
  }
  return TransitionTo(EditorSessionState::Interactive, EditorSessionResultKind::Accepted,
                      "Discarded unflushed transaction");
}

auto EditorSessionService::HandleShutdown() -> EditorSessionResult {
  if (state_ == EditorSessionState::ShuttingDown) {
    return Reject("Already shutting down");
  }
  std::string error;
  if (!SealCurrentSession(/*persist_changes=*/true, /*start_background_save=*/true, &error)) {
    return TransitionTo(EditorSessionState::Failed, EditorSessionResultKind::Failed,
                        error.empty() ? "Failed to seal editor session for shutdown" : error);
  }
  identity_ = {};
  ResetActiveImageState();
  return TransitionTo(EditorSessionState::ShuttingDown, EditorSessionResultKind::StateChanged,
                      "Shutting down");
}

auto EditorSessionService::HandleViewChange(const EditorSessionIntent& intent)
    -> EditorSessionResult {
  const EditorRenderReason reason = intent.view_reason;
  // Qt Quick can display the submitted primary frame before its window-frame
  // composition acknowledgement reaches the session. During that interval the
  // user is already able to zoom, but the session is still Loading. Do not drop
  // a settled DetailRefresh from that visible frame: it is independent of the
  // first-composition state transition and the coordinator can serialize it
  // after any remaining primary work.
  const bool               detail_from_visible_loading_frame =
      reason == EditorRenderReason::DetailRefresh && state_ == EditorSessionState::Loading &&
      image_acquired_ && first_frame_completed_ && first_frame_submitted_;
  if (state_ != EditorSessionState::Interactive && !detail_from_visible_loading_frame) {
    return Reject("View change requires an interactive session");
  }
  if (!has_image()) {
    return Reject("View change requires an open image");
  }

  // Phase 5D D2 generation policy. A content-changing geometry change (crop/
  // rotation) advances the render generation and cancels obsolete full-frame
  // work. Pure view transforms (zoom/pan/resize) and detail refresh advance only
  // the view generation: full-frame renders survive (the renderer re-samples
  // them under the new view) and only view-dependent DetailPatch work is
  // cancelled by the view-generation advance.
  if (reason == EditorRenderReason::CropRotate) {
    ++identity_.render_generation;
  } else {
    ++identity_.view_generation;
  }
  if (dependencies_.render) {
    dependencies_.render->SetActiveGenerations(
        identity_.session_generation, identity_.render_generation, identity_.view_generation);
  }

  auto intent_opt = MakeRenderIntent(reason);
  if (!intent_opt) {
    return Reject("View change render intent could not be built");
  }
  // Phase 5D D5: attach the requested region to DetailRefresh intents. Full-frame
  // view changes carry no region; the production scheduler still loads the ROI
  // from the sink at render time for DetailPatch work.
  intent_opt->view_region =
      (reason == EditorRenderReason::DetailRefresh) ? intent.view_region : std::nullopt;

  EditorRenderResult routed;
  if (dependencies_.render) {
    routed = dependencies_.render->Submit(*intent_opt);
  } else {
    routed.kind    = EditorRenderResultKind::Failed;
    routed.message = "Render submit port unavailable";
  }

  EditorSessionResult result;
  result.state    = state_;
  result.identity = identity_;
  switch (routed.kind) {
    case EditorRenderResultKind::RequestAccepted:
      // Coordinator scheduled a pipeline task (InteractivePrimary for CropRotate,
      // DetailPatch for DetailRefresh). ZoomPan/Resize never reach here: the
      // coordinator reuses the current frame and returns Reused instead.
      result.kind              = EditorSessionResultKind::RenderRouted;
      result.render_request_id = routed.request_id;
      result.message           = "View change render routed";
      break;
    case EditorRenderResultKind::Reused:
      // Coordinator decision (D2): the current full frame covers this view
      // change; the viewport re-samples it via synchronize(). No pipeline task.
      result.kind    = EditorSessionResultKind::Accepted;
      result.message = "View change reused current frame";
      break;
    default:
      // Rejected/Failed: the session stays Interactive and the prior frame
      // remains valid. Surface the reason without a state transition.
      result.kind    = EditorSessionResultKind::Rejected;
      result.message = routed.message.empty() ? "View change render rejected" : routed.message;
      break;
  }
  return Emit(std::move(result));
}

auto EditorSessionService::Submit(const EditorSessionIntent& intent) -> EditorSessionResult {
  std::scoped_lock lock(mutex_);
  switch (intent.kind) {
    case EditorSessionIntentKind::Open:
      return HandleOpenOrSwitch(intent, /*is_switch=*/false);
    case EditorSessionIntentKind::Switch:
      return HandleOpenOrSwitch(intent, /*is_switch=*/true);
    case EditorSessionIntentKind::Close:
      return HandleClose(intent.persist_changes);
    case EditorSessionIntentKind::Patch:
      return HandlePatch(intent, /*settled=*/false);
    case EditorSessionIntentKind::CommitAdjustment:
      return HandlePatch(intent, /*settled=*/true);
    case EditorSessionIntentKind::Undo:
      return HandleUndoRedo(/*undo=*/true);
    case EditorSessionIntentKind::Redo:
      return HandleUndoRedo(/*undo=*/false);
    case EditorSessionIntentKind::Discard:
      return HandleDiscard();
    case EditorSessionIntentKind::ViewChange:
      return HandleViewChange(intent);
    case EditorSessionIntentKind::Shutdown:
      return HandleShutdown();
  }
  return Reject("Unknown session intent");
}

auto EditorSessionService::Open(sl_element_id_t element_id, image_id_t image_id)
    -> EditorSessionResult {
  EditorSessionIntent intent;
  intent.kind       = EditorSessionIntentKind::Open;
  intent.element_id = element_id;
  intent.image_id   = image_id;
  return Submit(intent);
}

auto EditorSessionService::Switch(sl_element_id_t element_id, image_id_t image_id)
    -> EditorSessionResult {
  EditorSessionIntent intent;
  intent.kind       = EditorSessionIntentKind::Switch;
  intent.element_id = element_id;
  intent.image_id   = image_id;
  return Submit(intent);
}

auto EditorSessionService::Close(bool persist_changes) -> EditorSessionResult {
  EditorSessionIntent intent;
  intent.kind            = EditorSessionIntentKind::Close;
  intent.persist_changes = persist_changes;
  return Submit(intent);
}

auto EditorSessionService::Patch(EditorAdjustmentPatch patch) -> EditorSessionResult {
  EditorSessionIntent intent;
  intent.kind  = EditorSessionIntentKind::Patch;
  intent.patch = std::move(patch);
  return Submit(intent);
}

auto EditorSessionService::CommitAdjustment(EditorAdjustmentPatch patch) -> EditorSessionResult {
  EditorSessionIntent intent;
  intent.kind  = EditorSessionIntentKind::CommitAdjustment;
  intent.patch = std::move(patch);
  return Submit(intent);
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
  return Submit(EditorSessionIntent{EditorSessionIntentKind::Undo});
}

auto EditorSessionService::Redo() -> EditorSessionResult {
  return Submit(EditorSessionIntent{EditorSessionIntentKind::Redo});
}

auto EditorSessionService::RecordFinalizedEdit(const EditTransaction& transaction,
                                               std::string*           error) -> bool {
  std::scoped_lock lock(mutex_);
  if (!dependencies_.journal || state_ != EditorSessionState::Interactive || !has_image()) {
    if (error) {
      *error = "Finalized edit requires an active journaled image";
    }
    return false;
  }
  return dependencies_.journal->RecordEdit(identity_.element_id, identity_.session_generation,
                                           transaction, error);
}

auto EditorSessionService::RecordHistoryCursorMove(std::uint64_t from_cursor,
                                                   std::uint64_t to_cursor, std::string* error)
    -> bool {
  std::scoped_lock lock(mutex_);
  if (!dependencies_.journal || state_ != EditorSessionState::Interactive || !has_image()) {
    if (error) {
      *error = "History cursor move requires an active journaled image";
    }
    return false;
  }
  if (from_cursor == to_cursor) {
    return true;
  }
  return dependencies_.journal->RecordCursorMove(identity_.element_id, identity_.session_generation,
                                                 from_cursor, to_cursor, error);
}

auto EditorSessionService::RecordTimelineRewrite(const Hash128&         expected_timeline_hash,
                                                 const Hash128&         discarded_tail_hash,
                                                 std::uint64_t          retained_cursor,
                                                 const EditTransaction& replacement,
                                                 std::string*           error) -> bool {
  std::scoped_lock lock(mutex_);
  if (!dependencies_.journal || state_ != EditorSessionState::Interactive || !has_image()) {
    if (error) {
      *error = "Timeline rewrite requires an active journaled image";
    }
    return false;
  }
  return dependencies_.journal->RecordRewriteTimeline(
      identity_.element_id, identity_.session_generation, expected_timeline_hash,
      discarded_tail_hash, retained_cursor, replacement, error);
}

auto EditorSessionService::Discard() -> EditorSessionResult {
  return Submit(EditorSessionIntent{EditorSessionIntentKind::Discard});
}

auto EditorSessionService::Shutdown() -> EditorSessionResult {
  return Submit(EditorSessionIntent{EditorSessionIntentKind::Shutdown});
}

auto EditorSessionService::RequestViewChange(EditorRenderReason                  reason,
                                             std::optional<ViewportRenderRegion> region)
    -> EditorSessionResult {
  EditorSessionIntent intent;
  intent.kind        = EditorSessionIntentKind::ViewChange;
  intent.view_reason = reason;
  intent.view_region = std::move(region);
  return Submit(intent);
}

auto EditorSessionService::CoordinatorBusy() const -> bool {
  if (!dependencies_.render) {
    return false;
  }
  const auto diag = dependencies_.render->diagnostics();
  return diag.has_inflight || diag.pending_count > 0;
}

auto EditorSessionService::render_busy() const -> bool {
  // Do not hold the service mutex while querying the coordinator. dependencies_
  // is immutable after construction, and the coordinator observer runs outside
  // the coordinator data mutex, so this cannot deadlock against NotifyRenderResult.
  return CoordinatorBusy();
}

void EditorSessionService::NotifyImageAcquired(std::uint64_t session_generation, bool success,
                                               std::string message) {
  std::scoped_lock lock(mutex_);
  // Stale completions for a prior open must not mutate the current session.
  if (session_generation != identity_.session_generation) {
    return;
  }
  if (state_ != EditorSessionState::Loading && state_ != EditorSessionState::Acquiring &&
      state_ != EditorSessionState::Switching) {
    return;
  }
  if (!success) {
    ReleaseGuards();
    image_acquired_ = false;
    TransitionTo(EditorSessionState::Failed, EditorSessionResultKind::Failed,
                 message.empty() ? "Image acquisition failed" : std::move(message));
    return;
  }
  // Stay Loading until the matching first frame is submitted and presented.
  image_acquired_ = true;
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::Accepted;
  result.state    = state_;
  result.identity = identity_;
  result.message = message.empty() ? "Image acquired; waiting for first frame" : std::move(message);
  Emit(std::move(result));
  TryEnterInteractiveFromFirstFrame({});
}

void EditorSessionService::NotifySaveFinished(std::uint64_t session_generation, bool success,
                                              std::string message) {
  std::scoped_lock lock(mutex_);
  auto             it = std::find_if(pending_saves_.begin(), pending_saves_.end(),
                                     [session_generation](const PendingSave& save) {
                           return save.session_generation == session_generation;
                         });
  if (it == pending_saves_.end()) {
    return;
  }
  const std::uint64_t task_id = it->task_id;
  pending_saves_.erase(it);
  if (dependencies_.tasks) {
    dependencies_.tasks->EndTask(task_id, success, message);
  }
  const bool is_current_session = session_generation == identity_.session_generation &&
                                  identity_.element_id != 0 && identity_.image_id != 0;
  if (!is_current_session) {
    // The task belongs to a sealed image. Its background task still needs to
    // reach a terminal state, but its result must not be published as if it
    // described the image that is active now.
    return;
  }
  if (!success) {
    const std::string failure = message.empty() ? "Save failed" : message;
    last_error_               = failure;
    EditorSessionResult result;
    result.kind     = EditorSessionResultKind::Failed;
    result.state    = state_;
    result.identity = identity_;
    result.task_id  = task_id;
    result.message  = failure;
    Emit(std::move(result));
    return;
  }
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::SaveFinished;
  result.state    = state_;
  result.identity = identity_;
  result.task_id  = task_id;
  result.message  = std::move(message);
  Emit(std::move(result));
}

auto EditorSessionService::MatchesActiveFirstFrame(const EditorRenderResult& render_result) const
    -> bool {
  if (first_frame_request_id_ == 0 || render_result.request_id != first_frame_request_id_) {
    return false;
  }
  const auto& intent = render_result.intent;
  return intent.element_id == identity_.element_id && intent.image_id == identity_.image_id &&
         intent.session_generation == identity_.session_generation &&
         intent.render_generation == identity_.render_generation &&
         intent.view_generation == identity_.view_generation;
}

void EditorSessionService::TryEnterInteractiveFromFirstFrame(
    const EditorRenderResult& /*render_result*/) {
  if (state_ != EditorSessionState::Loading && state_ != EditorSessionState::Acquiring &&
      state_ != EditorSessionState::Switching) {
    return;
  }
  if (!image_acquired_ || !first_frame_completed_ || !first_frame_submitted_ ||
      !first_frame_presented_) {
    return;
  }
  TransitionTo(EditorSessionState::Interactive, EditorSessionResultKind::ImageReady,
               "First frame presented");
  // InteractivePrimary is visible; queue the normal QualityBase follow-up.
  // DetailPatch is never a prerequisite for the first visible frame.
  RouteQualityBaseFollowUp();
}

void EditorSessionService::NotifyRenderResult(const EditorRenderResult& render_result) {
  std::scoped_lock lock(mutex_);
  // Ignore results for a different image/session.
  if (render_result.intent.session_generation != 0 &&
      render_result.intent.session_generation != identity_.session_generation) {
    return;
  }
  if (render_result.intent.image_id != 0 && render_result.intent.image_id != identity_.image_id) {
    return;
  }
  if (render_result.intent.element_id != 0 &&
      render_result.intent.element_id != identity_.element_id) {
    return;
  }

  if (render_result.kind == EditorRenderResultKind::Failed) {
    std::cerr << "[dbg2] NotifyRenderResult FAILED state=" << static_cast<int>(state_)
              << " matches_first=" << MatchesActiveFirstFrame(render_result)
              << " msg=" << render_result.message << "\n";
    if (state_ == EditorSessionState::Loading && MatchesActiveFirstFrame(render_result)) {
      TransitionTo(EditorSessionState::Failed, EditorSessionResultKind::Failed,
                   render_result.message.empty() ? "Render failed" : render_result.message);
    }
    return;
  }

  // First-frame gate: require matching generations and ordered complete→submit→present.
  if (MatchesActiveFirstFrame(render_result)) {
    if (render_result.kind == EditorRenderResultKind::RenderCompleted) {
      first_frame_completed_ = true;
    } else if (render_result.kind == EditorRenderResultKind::FrameSubmitted) {
      if (!first_frame_completed_) {
        return;
      }
      first_frame_submitted_ = true;
    } else if (render_result.kind == EditorRenderResultKind::FramePresented) {
      if (!first_frame_completed_ || !first_frame_submitted_) {
        return;
      }
      if (first_frame_presented_) {
        return;
      }
      first_frame_presented_ = true;
      if (first_frame_route_time_.has_value()) {
        const auto elapsed   = std::chrono::steady_clock::now() - *first_frame_route_time_;
        first_frame_time_ms_ = std::chrono::duration<double, std::milli>(elapsed).count();
      }
      TryEnterInteractiveFromFirstFrame(render_result);
    }
    return;
  }

  // Non-first-frame results: still require generation match for presentation side-effects.
  if (render_result.kind == EditorRenderResultKind::FramePresented) {
    if (render_result.intent.render_generation != identity_.render_generation ||
        render_result.intent.view_generation != identity_.view_generation) {
      return;
    }
  }

  // Phase 5D D6: announce render-busy transitions so QML spinners reflect
  // coordinator in-flight/pending state even when the session state itself does
  // not change (e.g. a view-change render starts or completes while Interactive).
  const bool busy = CoordinatorBusy();
  if (busy != last_notified_render_busy_) {
    last_notified_render_busy_ = busy;
    NotifyChange();
  }
}

}  // namespace alcedo
