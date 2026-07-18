//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_session_service.hpp"

#include <utility>

namespace alcedo {

EditorSessionService::EditorSessionService(Dependencies dependencies)
    : dependencies_(std::move(dependencies)) {}

void EditorSessionService::SetResultObserver(ResultObserver observer) {
  observer_ = std::move(observer);
}

void EditorSessionService::SetPresentationSinkId(PresentationSinkId sink_id) {
  presentation_sink_id_ = sink_id;
}

auto EditorSessionService::Emit(EditorSessionResult result) -> EditorSessionResult {
  results_.push_back(result);
  if (observer_) {
    observer_(results_.back());
  }
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
  if (!has_image() && state_ != EditorSessionState::Loading &&
      state_ != EditorSessionState::Acquiring) {
    return std::nullopt;
  }
  EditorRenderIntent intent;
  intent.element_id          = identity_.element_id;
  intent.image_id            = identity_.image_id;
  intent.session_generation  = identity_.session_generation;
  intent.render_generation   = identity_.render_generation;
  intent.view_generation     = identity_.view_generation;
  intent.reason              = reason;
  intent.quality             = DefaultQualityForReason(reason);
  intent.priority            = DefaultPriorityForReason(reason);
  intent.frame_role          = FrameRoleForQuality(intent.quality);
  intent.replacement_key     = DefaultReplacementKey(intent.quality);
  intent.presentation_sink_id = presentation_sink_id_;
  intent.cancellation        = std::make_shared<EditorRenderCancellationToken>();
  return intent;
}

auto EditorSessionService::RouteInitialRender(EditorRenderReason reason) -> std::uint64_t {
  if (!dependencies_.render) {
    return 0;
  }
  auto intent = MakeRenderIntent(reason);
  if (!intent) {
    return 0;
  }
  dependencies_.render->SetActiveGenerations(identity_.session_generation,
                                             identity_.render_generation,
                                             identity_.view_generation);
  const EditorRenderResult routed = dependencies_.render->Submit(*intent);
  if (routed.kind == EditorRenderResultKind::RequestAccepted) {
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

auto EditorSessionService::HandleOpenOrSwitch(const EditorSessionIntent& intent, bool is_switch)
    -> EditorSessionResult {
  if (state_ == EditorSessionState::ShuttingDown) {
    return Reject("Cannot open while shutting down");
  }

  const bool open_empty = intent.element_id == 0 || intent.image_id == 0;
  if (open_empty) {
    if (dependencies_.render && identity_.session_generation != 0) {
      dependencies_.render->CancelSession(identity_.session_generation);
    }
    ReleaseGuards();
    // Clear image identity but keep session_generation so the next Open still
    // advances past it (A → empty → B must not reuse generation A).
    identity_.element_id = 0;
    identity_.image_id   = 0;
    pending_save_ = false;
    pending_save_generation_ = 0;
    return TransitionTo(EditorSessionState::NoImage, EditorSessionResultKind::StateChanged,
                        "No image selected");
  }

  // Seal prior session: cancel outdated renders; optional save barrier when switching.
  if (identity_.session_generation != 0 &&
      (identity_.element_id != intent.element_id || identity_.image_id != intent.image_id)) {
    if (dependencies_.render) {
      dependencies_.render->CancelSession(identity_.session_generation);
    }
    if (is_switch || state_ == EditorSessionState::Interactive ||
        state_ == EditorSessionState::Saving) {
      pending_save_            = true;
      pending_save_generation_ = identity_.session_generation;
      if (dependencies_.tasks) {
        dependencies_.tasks->BeginTask("editor_save", identity_.element_id);
      }
      if (dependencies_.journal) {
        std::string journal_error;
        dependencies_.journal->AppendBarrier(identity_.element_id, identity_.session_generation,
                                             &journal_error);
      }
    }
    ReleaseGuards();
  }

  ++identity_.session_generation;
  identity_.element_id        = intent.element_id;
  identity_.image_id          = intent.image_id;
  identity_.render_generation = identity_.session_generation;
  identity_.view_generation   = 1;

  const EditorSessionState acquire_state =
      is_switch ? EditorSessionState::Switching : EditorSessionState::Acquiring;
  TransitionTo(acquire_state, EditorSessionResultKind::StateChanged,
               is_switch ? "Switching image" : "Acquiring image");

  std::string error;
  if (!AcquireGuards(intent.element_id, &error)) {
    identity_.element_id = 0;
    identity_.image_id   = 0;
    return TransitionTo(EditorSessionState::Failed, EditorSessionResultKind::Failed,
                        error.empty() ? "Failed to acquire pipeline/history guards" : error);
  }

  TransitionTo(EditorSessionState::Loading, EditorSessionResultKind::StateChanged, "Loading image");
  // Phase 5A: route the initial interactive render intent immediately so open
  // always creates a render intent. Phase 5B owns real frame presentation.
  RouteInitialRender(is_switch ? EditorRenderReason::ImageSwitch
                               : EditorRenderReason::InitialFrame);
  return results_.back();
}

auto EditorSessionService::HandlePatch(const EditorSessionIntent& intent, bool settled)
    -> EditorSessionResult {
  if (state_ != EditorSessionState::Interactive && state_ != EditorSessionState::Loading) {
    return Reject("Patch requires an active interactive or loading session");
  }
  if (!has_image()) {
    return Reject("Patch requires an open image");
  }
  ++identity_.render_generation;
  if (dependencies_.render) {
    dependencies_.render->SetActiveGenerations(identity_.session_generation,
                                               identity_.render_generation,
                                               identity_.view_generation);
  }
  const auto reason = settled ? EditorRenderReason::SettledAdjustment
                              : EditorRenderReason::InteractiveAdjustment;
  const auto request_id = RouteInitialRender(reason);
  EditorSessionResult result;
  result.kind              = EditorSessionResultKind::RenderRouted;
  result.state             = state_;
  result.identity          = identity_;
  result.render_request_id = request_id;
  result.message           = intent.patch_key;
  return Emit(std::move(result));
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
  ++identity_.render_generation;
  RouteInitialRender(EditorRenderReason::UndoRedo);
  return TransitionTo(EditorSessionState::Interactive, EditorSessionResultKind::Accepted,
                      undo ? "Undo applied" : "Redo applied");
}

auto EditorSessionService::HandleDiscard() -> EditorSessionResult {
  if (!has_image() && state_ != EditorSessionState::Failed) {
    return Reject("Discard requires a session with an image");
  }
  if (dependencies_.journal) {
    std::string error;
    if (!dependencies_.journal->DiscardUnflushed(identity_.element_id, &error)) {
      return TransitionTo(EditorSessionState::Failed, EditorSessionResultKind::Failed,
                          error.empty() ? "Discard failed" : error);
    }
  }
  return TransitionTo(state_ == EditorSessionState::Failed ? EditorSessionState::Interactive
                                                           : state_,
                      EditorSessionResultKind::Accepted, "Discarded unflushed transaction");
}

auto EditorSessionService::HandleShutdown() -> EditorSessionResult {
  if (state_ == EditorSessionState::ShuttingDown) {
    return Reject("Already shutting down");
  }
  if (dependencies_.render && identity_.session_generation != 0) {
    dependencies_.render->CancelSession(identity_.session_generation);
  }
  if (has_image() && dependencies_.journal) {
    std::string error;
    dependencies_.journal->AppendBarrier(identity_.element_id, identity_.session_generation,
                                         &error);
  }
  ReleaseGuards();
  identity_ = {};
  pending_save_ = false;
  pending_save_generation_ = 0;
  return TransitionTo(EditorSessionState::ShuttingDown, EditorSessionResultKind::StateChanged,
                      "Shutting down");
}

auto EditorSessionService::Submit(const EditorSessionIntent& intent) -> EditorSessionResult {
  switch (intent.kind) {
    case EditorSessionIntentKind::Open:
      return HandleOpenOrSwitch(intent, /*is_switch=*/false);
    case EditorSessionIntentKind::Switch:
      return HandleOpenOrSwitch(intent, /*is_switch=*/true);
    case EditorSessionIntentKind::Patch:
      return HandlePatch(intent, /*settled=*/false);
    case EditorSessionIntentKind::GestureCommit:
      return HandlePatch(intent, /*settled=*/true);
    case EditorSessionIntentKind::Undo:
      return HandleUndoRedo(/*undo=*/true);
    case EditorSessionIntentKind::Redo:
      return HandleUndoRedo(/*undo=*/false);
    case EditorSessionIntentKind::Discard:
      return HandleDiscard();
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

auto EditorSessionService::Patch(std::string patch_key) -> EditorSessionResult {
  EditorSessionIntent intent;
  intent.kind      = EditorSessionIntentKind::Patch;
  intent.patch_key = std::move(patch_key);
  return Submit(intent);
}

auto EditorSessionService::GestureCommit(std::string patch_key) -> EditorSessionResult {
  EditorSessionIntent intent;
  intent.kind      = EditorSessionIntentKind::GestureCommit;
  intent.patch_key = std::move(patch_key);
  return Submit(intent);
}

auto EditorSessionService::Undo() -> EditorSessionResult {
  return Submit(EditorSessionIntent{EditorSessionIntentKind::Undo});
}

auto EditorSessionService::Redo() -> EditorSessionResult {
  return Submit(EditorSessionIntent{EditorSessionIntentKind::Redo});
}

auto EditorSessionService::Discard() -> EditorSessionResult {
  return Submit(EditorSessionIntent{EditorSessionIntentKind::Discard});
}

auto EditorSessionService::Shutdown() -> EditorSessionResult {
  return Submit(EditorSessionIntent{EditorSessionIntentKind::Shutdown});
}

void EditorSessionService::NotifyImageAcquired(std::uint64_t session_generation, bool success,
                                               std::string message) {
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
    TransitionTo(EditorSessionState::Failed, EditorSessionResultKind::Failed,
                 message.empty() ? "Image acquisition failed" : std::move(message));
    return;
  }
  TransitionTo(EditorSessionState::Interactive, EditorSessionResultKind::ImageReady,
               message.empty() ? "Image ready" : std::move(message));
}

void EditorSessionService::NotifySaveFinished(std::uint64_t session_generation, bool success,
                                              std::string message) {
  if (!pending_save_ || session_generation != pending_save_generation_) {
    // Completions for non-current save generations are ignored (reordered async).
    return;
  }
  pending_save_            = false;
  pending_save_generation_ = 0;
  if (dependencies_.tasks) {
    dependencies_.tasks->EndTask(0, success, message);
  }
  if (!success && state_ != EditorSessionState::ShuttingDown) {
    // Save failure is recorded but does not force Failed if a new image is interactive.
    last_error_ = message.empty() ? "Save failed" : message;
    EditorSessionResult result;
    result.kind     = EditorSessionResultKind::Failed;
    result.state    = state_;
    result.identity = identity_;
    result.message  = last_error_;
    Emit(std::move(result));
    return;
  }
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::SaveFinished;
  result.state    = state_;
  result.identity = identity_;
  result.message  = std::move(message);
  Emit(std::move(result));
}

void EditorSessionService::NotifyRenderResult(const EditorRenderResult& render_result) {
  // Presentation is a separate event from pipeline task completion. Session UI
  // follows these results instead of assuming a completed task is already visible.
  if (render_result.intent.session_generation != identity_.session_generation) {
    return;
  }
  if (render_result.kind == EditorRenderResultKind::Failed) {
    if (state_ == EditorSessionState::Loading) {
      TransitionTo(EditorSessionState::Failed, EditorSessionResultKind::Failed,
                   render_result.message.empty() ? "Render failed" : render_result.message);
    }
    return;
  }
  if (render_result.kind == EditorRenderResultKind::FramePresented &&
      state_ == EditorSessionState::Loading) {
    TransitionTo(EditorSessionState::Interactive, EditorSessionResultKind::ImageReady,
                 "First frame presented");
  }
}

}  // namespace alcedo
