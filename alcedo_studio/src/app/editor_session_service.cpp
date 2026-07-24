//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_session_service.hpp"

#include <utility>

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
                  dependencies_.checkpoint_store.get(), dependencies_.history.get()) {}

EditorSessionService::~EditorSessionService() { save_service_.CancelAndWait(); }

void EditorSessionService::SetResultObserver(ResultObserver observer) {
  std::scoped_lock lock(results_mutex_);
  observer_ = std::move(observer);
}

void EditorSessionService::SetChangeNotifier(ChangeNotifier notifier) {
  change_notifier_ = std::move(notifier);
}

auto EditorSessionService::Emit(EditorSessionResult result) -> EditorSessionResult {
  {
    std::scoped_lock lock(results_mutex_);
    results_.push_back(result);
  }
  if (observer_) {
    observer_(result);
  }
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
  lifecycle_.Fail(message);
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::Failed;
  result.state    = lifecycle_.state();
  result.identity = lifecycle_.identity();
  result.message  = std::move(message);
  return Emit(std::move(result));
}

auto EditorSessionService::Open(sl_element_id_t element_id, image_id_t image_id)
    -> EditorSessionResult {
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
  return Emit(std::move(result));
}

auto EditorSessionService::Switch(sl_element_id_t element_id, image_id_t image_id)
    -> EditorSessionResult {
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
  return Emit(std::move(result));
}

auto EditorSessionService::Close(bool persist_changes) -> EditorSessionResult {
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
  return Emit(std::move(result));
}

auto EditorSessionService::Discard() -> EditorSessionResult {
  const auto state = lifecycle_.state();
  if (state != EditorSessionState::Interactive && state != EditorSessionState::Failed) {
    return Reject("Discard requires an image with an active history session");
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
  return Emit(std::move(result));
}

auto EditorSessionService::Shutdown() -> EditorSessionResult {
  if (lifecycle_.state() == EditorSessionState::ShuttingDown) {
    return Reject("Already shutting down");
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

auto EditorSessionService::RecordFinalizedEdit(const EditTransaction& transaction,
                                               std::string*           error) -> bool {
  return edit_.RecordFinalizedEdit(transaction, lifecycle_.identity(), error);
}

auto EditorSessionService::RecordHistoryCursorMove(std::uint64_t from_cursor,
                                                   std::uint64_t to_cursor, std::string* error)
    -> bool {
  return edit_.RecordHistoryCursorMove(from_cursor, to_cursor, lifecycle_.identity(), error);
}

auto EditorSessionService::RecordTimelineRewrite(const Hash128&         expected_timeline_hash,
                                                 const Hash128&         discarded_tail_hash,
                                                 std::uint64_t          retained_cursor,
                                                 const EditTransaction& replacement,
                                                 std::string*           error) -> bool {
  return edit_.RecordTimelineRewrite(expected_timeline_hash, discarded_tail_hash, retained_cursor,
                                     replacement, lifecycle_.identity(), error);
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
