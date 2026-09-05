//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/editor_session_service.hpp"

#include <algorithm>
#include <utility>

#include "app/editor_action_policy.hpp"
#include "app/editor_mini_git_materializer.hpp"
#include "app/editor_session_edit_controller.hpp"
#include "app/editor_session_lifecycle.hpp"
#include "app/editor_session_navigation_controller.hpp"
#include "app/editor_session_render_controller.hpp"

namespace alcedo {

namespace {

/// Adapts save-service completion delivery to the session queue. Save ports
/// may finish on any worker stack; this adapter ensures their callback enters
/// the same serialized completion drain as render and image-load messages.
class SessionQueueCompletionExecutor final : public IEditorSessionCommandExecutor {
 public:
  explicit SessionQueueCompletionExecutor(EditorSessionCommandQueue& queue) : queue_(queue) {}

  void Post(std::function<void()> task) override { queue_.PostCompletion(std::move(task)); }

  [[nodiscard]] auto IsOwnerThread() const -> bool override { return queue_.IsOwnerThread(); }

 private:
  EditorSessionCommandQueue& queue_;
};

}  // namespace

EditorSessionService::EditorSessionService(Dependencies dependencies)
    : dependencies_(std::move(dependencies)),
      command_queue_(dependencies_.command_executor),
      lifecycle_(
          EditorSessionLifecycle::Dependencies{dependencies_.pipeline, dependencies_.history}),
      save_service_(EditorSaveCheckpointService::Dependencies{
          dependencies_.journal, dependencies_.checkpoint_store, dependencies_.thumbnails,
          dependencies_.tasks, std::make_shared<SessionQueueCompletionExecutor>(command_queue_),
          dependencies_.save_coordinator}),
      render_(EditorSessionRenderController::Dependencies{
          dependencies_.render,
          [this](const EditorRenderEvent& event) {
            EditorSessionCompletion completion;
            completion.kind                 = EditorSessionCompletionKind::RenderResult;
            completion.operation.command_id = event.operation_id;
            completion.request_id           = event.request_id;
            completion.render_event         = event;
            completion.message              = event.message;
            PostCompletion(std::move(completion));
          }}),
      edit_(
          EditorSessionEditController::Dependencies{dependencies_.history, dependencies_.journal}),
      navigation_(lifecycle_, save_service_, render_, dependencies_.journal.get(),
                  dependencies_.checkpoint_store.get(), dependencies_.history.get(),
                  &navigation_state_) {
  navigation_.SetOwnerPoster([this](std::function<void()> task) {
    command_queue_.PostCompletion(std::move(task));
  });
  navigation_.SetCompletionNotifier([this](const NavigationCompletion& completion) {
    EditorSessionCompletion posted;
    posted.kind                 = EditorSessionCompletionKind::NavigationFinished;
    posted.operation.command_id = completion.ticket.operation_id;
    posted.request_id           = completion.ticket.request_id;
    posted.task_id              = completion.ticket.task_id;
    posted.image_load_request   = completion.ticket.image_load_request_id;
    posted.element_id           = completion.ticket.element_id;
    posted.navigation_success   = completion.success;
    posted.retained_image       = completion.retained_image;
    posted.message              = completion.message;
    PostCompletion(std::move(posted));
  });
}

EditorSessionService::~EditorSessionService() { save_service_.CancelAndWait(); }

void EditorSessionService::DrainCommandQueueForTests() {
  const auto manual =
      std::dynamic_pointer_cast<EditorSessionManualCommandExecutor>(command_queue_.executor());
  if (manual && manual->IsOwnerThread()) {
    manual->DrainAll();
  }
}

auto EditorSessionService::SubmitCommand(EditorSessionCommand command, CommandReducer reducer)
    -> EditorSessionResult {
  if (!reducer) {
    return {};
  }
  if (reducing_command_) {
    return reducer(command);
  }

  auto       reduced_result = std::make_shared<std::optional<EditorSessionResult>>();
  const auto submission     = command_queue_.Submit(
      std::move(command),
      [this, reducer = std::move(reducer), reduced_result](EditorSessionCommand queued) mutable {
        const auto previous_operation = current_operation_id_;
        const auto previous_reducing  = reducing_command_;
        current_operation_id_         = queued.operation.command_id;
        navigation_.SetOperationId(current_operation_id_);
        reducing_command_ = true;
        BeginPublication();
        if (const auto action = EditorActionPolicy::ActionForCommand(queued.kind)) {
          const auto decision = EditorActionPolicy::Evaluate(
              *action, EditorCommandContext{active_leases_}, BuildActionInputs());
          if (!decision.allowed) {
            EditorSessionResult rejected;
            rejected.kind         = EditorSessionResultKind::Rejected;
            rejected.operation_id = queued.operation.command_id;
            rejected.state        = lifecycle_.state();
            rejected.identity     = lifecycle_.identity();
            rejected.message      = decision.reason.empty() ? "Editor command rejected by policy"
                                                            : decision.reason;
            *reduced_result = Emit(std::move(rejected));
            EndPublication();
            reducing_command_     = previous_reducing;
            current_operation_id_ = previous_operation;
            return;
          }
        }
        *reduced_result = reducer(queued);
        EndPublication();
        reducing_command_     = previous_reducing;
        current_operation_id_ = previous_operation;
      });

  if (!submission.accepted) {
    EditorSessionResult rejected;
    rejected.kind         = EditorSessionResultKind::Rejected;
    rejected.operation_id = submission.operation.command_id;
    rejected.message      = "Editor session command queue is shutting down";
    if (command_queue_.IsOwnerThread()) {
      rejected.state    = lifecycle_.state();
      rejected.identity = lifecycle_.identity();
    }
    return rejected;
  }
  if (reduced_result->has_value()) {
    return reduced_result->value();
  }

  EditorSessionResult queued_result;
  queued_result.kind         = EditorSessionResultKind::Accepted;
  queued_result.operation_id = submission.operation.command_id;
  queued_result.message      = "Editor session command queued";
  return queued_result;
}

void EditorSessionService::BeginPublication() { ++publication_depth_; }

void EditorSessionService::EndPublication() {
  if (publication_depth_ == 0) {
    return;
  }
  --publication_depth_;
  if (publication_depth_ == 0 && publication_dirty_) {
    publication_dirty_ = false;
    PublishActionAvailabilityIfChanged();
    NotifyChange();
  }
}

void EditorSessionService::PostCompletion(EditorSessionCompletion completion) {
  command_queue_.PostCompletion([this, completion = std::move(completion)]() mutable {
    const auto previous_operation = current_operation_id_;
    const auto previous_reducing  = reducing_command_;
    current_operation_id_         = completion.operation.command_id != 0
                                        ? completion.operation.command_id
                                        : completion.render_event.operation_id;
    navigation_.SetOperationId(current_operation_id_);
    reducing_command_ = true;
    BeginPublication();
    switch (completion.kind) {
      case EditorSessionCompletionKind::RenderResult:
        HandleRenderEvent(completion.render_event);
        break;
      case EditorSessionCompletionKind::NavigationFinished:
        HandleNavigationCompletion(NavigationCompletion{
            completion.navigation_success, completion.retained_image, completion.message,
            CheckpointTicket{completion.request_id, completion.operation.command_id,
                             completion.image_load_request, completion.element_id,
                             completion.task_id}});
        break;
      case EditorSessionCompletionKind::ImageStateLoaded:
        HandleImageAcquiredCompletion(completion);
        break;
      case EditorSessionCompletionKind::SaveCheckpointFinished:
        HandleSaveCheckpointCompletion(completion);
        break;
      default:
        break;
    }
    EndPublication();
    reducing_command_     = previous_reducing;
    current_operation_id_ = previous_operation;
  });
}

void EditorSessionService::HandleRenderEvent(const EditorRenderEvent& event) {
  if (event.kind == EditorRenderEventKind::BusyChanged) {
    NotifyChange();
    return;
  }

  EditorSessionResult result;
  result.operation_id = event.operation_id;
  switch (event.kind) {
    case EditorRenderEventKind::FirstFrameReady: {
      ReleaseLeasesByKind(EditorOperationLeaseKind::ImageLoad);
      ReleaseLeasesByKind(EditorOperationLeaseKind::ImageSwitch);
      ClearPendingPresentationTarget();
      const auto entered = lifecycle_.MarkFirstFrameReady();
      if (entered.has_value()) {
        result.kind     = EditorSessionResultKind::ImageReady;
        result.state    = EditorSessionState::Interactive;
        result.identity = *entered;
      } else {
        result.kind     = EditorSessionResultKind::StateChanged;
        result.state    = lifecycle_.state();
        result.identity = event.identity;
      }
      break;
    }
    case EditorRenderEventKind::RenderFailed:
      result.kind    = EditorSessionResultKind::Failed;
      result.message = event.message;
      break;
    case EditorRenderEventKind::RenderRouted:
      // The command handler (Open, navigation completion, or view change) is
      // the authority for the render-routed terminal and carries the request
      // id. This intermediate event must not publish a second terminal result
      // for the same accepted command.
      return;
    case EditorRenderEventKind::RenderReused:
      result.kind    = EditorSessionResultKind::Accepted;
      result.message = event.message;
      break;
    case EditorRenderEventKind::RenderRejected:
      result.kind    = EditorSessionResultKind::Rejected;
      result.message = event.message;
      break;
    case EditorRenderEventKind::BusyChanged:
      return;
  }
  result.state    = event.state;
  result.identity = event.identity;
  if (result.kind == EditorSessionResultKind::Failed) {
    lifecycle_.Fail(result.message.empty() ? "Render failed" : result.message);
    result.state    = lifecycle_.state();
    result.identity = lifecycle_.identity();
    BumpHistoryRevision();
  }
  Emit(std::move(result));
}

void EditorSessionService::HandleNavigationCompletion(const NavigationCompletion& completion) {
  EditorSessionResult result;
  result.operation_id = completion.ticket.operation_id;
  result.identity     = lifecycle_.identity();
  result.state        = lifecycle_.state();
  result.task_id      = completion.ticket.task_id;
  if (completion.success) {
    result.kind = render_.first_frame_request_id() != 0 ? EditorSessionResultKind::RenderRouted
                                                        : EditorSessionResultKind::Accepted;
    result.render_request_id = render_.first_frame_request_id();
    result.message           = completion.message;
    BumpHistoryRevision();
  } else {
    result.kind    = EditorSessionResultKind::Failed;
    result.message = completion.message;
  }
  Emit(std::move(result));
}

void EditorSessionService::HandleImageAcquiredCompletion(
    const EditorSessionCompletion& completion) {
  const auto identity = lifecycle_.identity();
  if (!lifecycle_.MatchesImageLoadRequest(completion.image_load_request)) {
    return;
  }
  const auto state = lifecycle_.state();
  if (state != EditorSessionState::Loading && state != EditorSessionState::Acquiring &&
      state != EditorSessionState::Switching) {
    return;
  }
  if (!completion.success) {
    lifecycle_.ReleaseGuards();
    ReleaseLeasesByKind(EditorOperationLeaseKind::ImageLoad);
    ReleaseLeasesByKind(EditorOperationLeaseKind::ImageSwitch);
    ClearPendingPresentationTarget();
    render_.ResetForNewImage();
    lifecycle_.Fail(completion.message.empty() ? "Image acquisition failed" : completion.message);
    EditorSessionResult result;
    result.operation_id = completion.operation.command_id;
    result.kind         = EditorSessionResultKind::Failed;
    result.state        = lifecycle_.state();
    result.identity     = lifecycle_.identity();
    result.message      = lifecycle_.last_error();
    BumpHistoryRevision();
    Emit(std::move(result));
    return;
  }
  render_.MarkImageAcquired();
  EditorSessionResult result;
  result.operation_id = completion.operation.command_id;
  result.kind         = EditorSessionResultKind::Accepted;
  result.state        = lifecycle_.state();
  result.identity     = identity;
  result.message =
      completion.message.empty() ? "Image acquired; waiting for first frame" : completion.message;
  Emit(std::move(result));
}

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
  if (result.operation_id == 0) {
    result.operation_id = current_operation_id_;
  }
  {
    std::scoped_lock lock(results_mutex_);
    results_.push_back(result);
  }
  NotifyResult(result);
  if (publication_depth_ != 0) {
    publication_dirty_ = true;
  } else {
    NotifyChange();
  }
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
  // Synchronous completion without a save checkpoint (no prior image or a
  // discard-and-continue path). Save-bounded navigations return above with
  // SaveStarted; their terminal arrives through the posted completion.
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::Accepted;
  result.state    = lifecycle_.state();
  result.identity = lifecycle_.identity();
  result.message  = outcome.message;
  if (render_.first_frame_request_id() != 0) {
    result.kind              = EditorSessionResultKind::RenderRouted;
    result.render_request_id = render_.first_frame_request_id();
  }
  BumpHistoryRevision();
  return Emit(std::move(result));
}

auto EditorSessionService::Open(sl_element_id_t element_id, image_id_t image_id)
    -> EditorSessionResult {
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind       = EditorSessionCommandKind::OpenImage;
    command.element_id = element_id;
    command.image_id   = image_id;
    return SubmitCommand(std::move(command), [this](const EditorSessionCommand& queued) {
      return Open(queued.element_id, queued.image_id);
    });
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
  // No prior image or same-image no-op: the navigation completed synchronously
  // without a save checkpoint. Save-bounded switches return above with
  // SaveStarted; the terminal for those arrives through the posted completion.
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
    SetPendingPresentationTarget(element_id, image_id, lifecycle_.active_image_load_request());
    AcquireLease(EditorOperationLeaseKind::ImageLoad, current_operation_id_, element_id, image_id,
                 lifecycle_.active_image_load_request(), "Image is loading");
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
  BumpHistoryRevision();
  return Emit(std::move(result));
}

auto EditorSessionService::CheckoutVersion(const version_ref_id_t& version_id)
    -> EditorSessionResult {
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind       = EditorSessionCommandKind::CheckoutVersion;
    command.version_id = version_id;
    return SubmitCommand(std::move(command), [this](const EditorSessionCommand& queued) {
      return CheckoutVersion(queued.version_id);
    });
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
  return snapshot;
}

auto EditorSessionService::active_version_id() const -> version_ref_id_t {
  if (!dependencies_.history || !lifecycle_.has_history_guard()) return {};
  std::string      error;
  version_ref_id_t version_id;
  if (!dependencies_.history->ReadActiveVersionId(lifecycle_.history_guard(), &version_id,
                                                   &error)) {
    return {};
  }
  return version_id;
}

auto EditorSessionService::pipeline_document() const -> const PipelineDocument* {
  if (!dependencies_.pipeline || !lifecycle_.has_image()) {
    return nullptr;
  }
  return dependencies_.pipeline->CurrentDocument(lifecycle_.identity().element_id);
}

auto EditorSessionService::adjustment_snapshot() const -> EditorRenderAdjustmentSnapshot {
  if (!dependencies_.history || !lifecycle_.has_history_guard()) {
    return {};
  }
  EditorRenderAdjustmentSnapshot snapshot;
  std::string                    error;
  auto& history = *dependencies_.history;
  if (!const_cast<IEditorHistoryPort&>(history).ReadAdjustmentSnapshot(
          lifecycle_.history_guard(), &snapshot, &error)) {
    return {};
  }
  return snapshot;
}

auto EditorSessionService::CreateRootVersion(std::string display_name) -> EditorSessionResult {
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind = EditorSessionCommandKind::CreateRootVersion;
    command.text = std::move(display_name);
    return SubmitCommand(std::move(command), [this](const EditorSessionCommand& queued) {
      return CreateRootVersion(queued.text);
    });
  }
  return FinishVersionNavigation(navigation_.RequestCreateRootVersion(std::move(display_name)));
}

auto EditorSessionService::BranchFromCommit(const commit_hash_t& commit_id,
                                            std::string display_name) -> EditorSessionResult {
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind      = EditorSessionCommandKind::BranchVersion;
    command.commit_id = commit_id;
    command.text      = std::move(display_name);
    return SubmitCommand(std::move(command), [this](const EditorSessionCommand& queued) {
      return BranchFromCommit(queued.commit_id, queued.text);
    });
  }
  return FinishVersionNavigation(
      navigation_.RequestBranchFromCommit(commit_id, std::move(display_name)));
}

auto EditorSessionService::RetrySave() -> EditorSessionResult {
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind = EditorSessionCommandKind::RetrySave;
    return SubmitCommand(std::move(command),
                         [this](const EditorSessionCommand&) { return RetrySave(); });
  }
  ReleaseLeasesByKind(EditorOperationLeaseKind::FailureRecovery);
  return FinishVersionNavigation(navigation_.RetrySaveAfterFailure());
}

auto EditorSessionService::DiscardAndContinue() -> EditorSessionResult {
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind = EditorSessionCommandKind::DiscardAndContinue;
    return SubmitCommand(std::move(command),
                         [this](const EditorSessionCommand&) { return DiscardAndContinue(); });
  }
  ReleaseLeasesByKind(EditorOperationLeaseKind::FailureRecovery);
  return FinishVersionNavigation(navigation_.DiscardAndContinueAfterFailure());
}

auto EditorSessionService::CancelPendingNavigation() -> EditorSessionResult {
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind = EditorSessionCommandKind::CancelPendingNavigation;
    return SubmitCommand(std::move(command),
                         [this](const EditorSessionCommand&) { return CancelPendingNavigation(); });
  }
  ReleaseLeasesByKind(EditorOperationLeaseKind::FailureRecovery);
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
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind       = EditorSessionCommandKind::RenameVersion;
    command.version_id = version_id;
    command.text       = std::move(display_name);
    return SubmitCommand(std::move(command), [this](const EditorSessionCommand& queued) {
      return RenameVersion(queued.version_id, queued.text);
    });
  }
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
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind       = EditorSessionCommandKind::RemoveVersion;
    command.version_id = version_id;
    return SubmitCommand(std::move(command), [this](const EditorSessionCommand& queued) {
      return RemoveVersion(queued.version_id);
    });
  }
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
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind             = EditorSessionCommandKind::ApplyPaste;
    command.transfer_package = package;
    command.text             = std::move(version_display_name);
    return SubmitCommand(std::move(command), [this](const EditorSessionCommand& queued) {
      return PasteAdjustments(queued.transfer_package, queued.text);
    });
  }
  if (lifecycle_.state() != EditorSessionState::Interactive || !dependencies_.history) {
    return Reject("Paste requires an interactive session");
  }
  AdjustmentPasteResult paste_result;
  std::string           error;
  if (!dependencies_.history->PasteLiveRootRelativeVersion(
          lifecycle_.history_guard(), package, std::move(version_display_name), &paste_result,
          &error)) {
    return Reject(error.empty() ? "Editor Paste failed" : std::move(error));
  }

  BumpHistoryRevision();
  AcquireLease(EditorOperationLeaseKind::PasteMaterialization, current_operation_id_,
               lifecycle_.identity().element_id, lifecycle_.identity().image_id,
               lifecycle_.active_image_load_request(), "Paste publication is in progress");
  auto started = StartHistoryCheckpoint("Adjustments pasted", true);
  if (started.kind == EditorSessionResultKind::Rejected ||
      started.kind == EditorSessionResultKind::Failed) {
    ReleaseLeasesByKind(EditorOperationLeaseKind::PasteMaterialization);
  }
  return started;
}


auto EditorSessionService::StartHistoryCheckpoint(std::string success_message, bool route_render)
    -> EditorSessionResult {
  pending_history_checkpoint_ = PendingHistoryCheckpoint{route_render, std::move(success_message)};
  auto result                 = StartHistoryCheckpointSave();
  if (result.kind == EditorSessionResultKind::Rejected ||
      result.kind == EditorSessionResultKind::Failed) {
    pending_history_checkpoint_.reset();
  }
  return result;
}

auto EditorSessionService::StartHistoryCheckpointSave() -> EditorSessionResult {
  if (!dependencies_.history || !lifecycle_.has_history_guard()) {
    return Reject("Editor history is unavailable");
  }
  if (navigation_.has_pending_action() || save_service_.active()) {
    return Reject("Editor save checkpoint is in progress");
  }

  const auto identity = lifecycle_.identity();
  if (dependencies_.journal) {
    std::string finalize_error;
    if (!dependencies_.journal->FinalizeEdit(identity.element_id,
                                             lifecycle_.active_image_load_request().value,
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

  SaveCheckpointRequest request;
  request.element_id         = identity.element_id;
  request.operation_id       = current_operation_id_;
  request.image_load_request_id = lifecycle_.active_image_load_request();
  request.capture            = std::move(capture);
  if (request.capture->has_journal_range()) {
    request.last_journal_sequence = request.capture->last_journal_sequence;
  }
  request.save_lock = std::move(save_lock);
  lifecycle_.BeginCheckpoint();
  AcquireLease(EditorOperationLeaseKind::SaveCheckpoint, current_operation_id_, identity.element_id,
               identity.image_id, lifecycle_.active_image_load_request(),
               "Editor save checkpoint is in progress");
  // The save service delivers this callback through the command executor, so
  // the lambda never runs inside Start: it posts one typed completion for the
  // session queue and returns.
  const auto ticket =
      save_service_.Start(std::move(request), [this](const SaveCheckpointResult& result) mutable {
        EditorSessionCompletion completion;
        completion.kind                  = EditorSessionCompletionKind::SaveCheckpointFinished;
        completion.request_id            = result.request_id;
        completion.operation.command_id  = result.operation_id;
        completion.image_load_request    = result.image_load_request_id;
        completion.task_id               = result.task_id;
        completion.success               = result.checkpoint_completed;
        completion.last_journal_sequence = result.last_journal_sequence;
        completion.message               = result.error;
        PostCompletion(std::move(completion));
      });

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

void EditorSessionService::HandleSaveCheckpointCompletion(
    const EditorSessionCompletion& completion) {
  // Shutdown cancels every in-flight checkpoint (CancelAndWait posts a
  // cancelled result). Handle that before load-request correlation: BeginShutdown
  // clears the active load id, and the cancellation must still publish one
  // terminal outcome without re-entering a recoverable failure state.
  if (lifecycle_.state() == EditorSessionState::ShuttingDown) {
    pending_history_checkpoint_.reset();
    ReleaseLeaseByCommandId(completion.operation.command_id);
    ReleaseLeasesByKind(EditorOperationLeaseKind::SaveCheckpoint);
    ReleaseLeasesByKind(EditorOperationLeaseKind::PasteMaterialization);
    EditorSessionResult published;
    published.kind     = EditorSessionResultKind::Failed;
    published.state    = lifecycle_.state();
    published.identity = lifecycle_.identity();
    published.task_id  = completion.task_id;
    published.message =
        completion.message.empty() ? "Editor save checkpoint cancelled" : completion.message;
    Emit(std::move(published));
    return;
  }

  // Stale completions from a previous image-load request are ignored without
  // touching the published snapshot.
  if (completion.image_load_request.valid() &&
      completion.image_load_request != lifecycle_.active_image_load_request()) {
    pending_history_checkpoint_.reset();
    return;
  }

  if (!completion.success) {
    pending_history_checkpoint_.reset();
    ReleaseLeasesByKind(EditorOperationLeaseKind::PasteMaterialization);
    lifecycle_.KeepCurrentAfterCheckpointFailure(
        completion.message.empty() ? "Editor save checkpoint failed" : completion.message);
    AcquireLease(EditorOperationLeaseKind::FailureRecovery, completion.operation.command_id,
                 lifecycle_.identity().element_id, lifecycle_.identity().image_id,
                 lifecycle_.active_image_load_request(), "Resolve the save failure to continue");
    EditorSessionResult published;
    published.kind     = EditorSessionResultKind::Failed;
    published.state    = lifecycle_.state();
    published.identity = lifecycle_.identity();
    published.task_id  = completion.task_id;
    published.message  = lifecycle_.last_error();
    BumpHistoryRevision();
    Emit(std::move(published));
    return;
  }

  if (completion.last_journal_sequence.has_value()) {
    std::string discard_error;
    (void)dependencies_.history->DiscardMaterializedJournalThrough(
        lifecycle_.history_guard(), *completion.last_journal_sequence, &discard_error);
  }
  lifecycle_.CompleteCheckpoint();
  ReleaseLeaseByCommandId(completion.operation.command_id);

  // The checkpoint materialized the active head to DuckDB without advancing
  // the in-memory materialized tuple. Mirror it for ordinary history saves.
  // Fail closed so a later version/checkout Persist does not see DuckDB ahead of
  // in-memory ImageEditState and report "persisted history changed".
  if (dependencies_.history != nullptr && lifecycle_.has_history_guard()) {
    std::string sync_error;
    if (!dependencies_.history->SyncMaterializedStateAfterCheckpoint(lifecycle_.history_guard(),
                                                                     &sync_error)) {
      pending_history_checkpoint_.reset();
      ReleaseLeasesByKind(EditorOperationLeaseKind::PasteMaterialization);
      EditorSessionResult failed;
      failed.kind     = EditorSessionResultKind::Failed;
      failed.state    = lifecycle_.state();
      failed.identity = lifecycle_.identity();
      failed.task_id  = completion.task_id;
      failed.message  = sync_error.empty() ? "Failed to sync materialized state after checkpoint"
                                           : std::move(sync_error);
      BumpHistoryRevision();
      Emit(std::move(failed));
      return;
    }
  }

  if (!pending_history_checkpoint_.has_value()) {
    return;
  }
  const auto marker = std::move(*pending_history_checkpoint_);
  pending_history_checkpoint_.reset();
  ReleaseLeasesByKind(EditorOperationLeaseKind::PasteMaterialization);

  EditorSessionResult published;
  published.identity = lifecycle_.identity();
  published.state    = lifecycle_.state();
  published.task_id  = completion.task_id;
  if (marker.route_render) {
    EditorRenderCommand command;
    command.reason =
        dependencies_.history->LastPublishedRenderReason().value_or(EditorRenderReason::InitialFrame);
    command.operation_id = current_operation_id_;
    render_.RouteInitialRender(command, lifecycle_.identity(),
                               lifecycle_.active_image_load_request());
    published.kind              = EditorSessionResultKind::RenderRouted;
    published.render_request_id = render_.first_frame_request_id();
  } else {
    published.kind = EditorSessionResultKind::Accepted;
  }
  published.state    = lifecycle_.state();
  published.identity = lifecycle_.identity();
  published.message  = marker.success_message;
  Emit(std::move(published));
}

auto EditorSessionService::Switch(sl_element_id_t element_id, image_id_t image_id)
    -> EditorSessionResult {
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind       = EditorSessionCommandKind::SelectImage;
    command.element_id = element_id;
    command.image_id   = image_id;
    return SubmitCommand(std::move(command), [this](const EditorSessionCommand& queued) {
      return Switch(queued.element_id, queued.image_id);
    });
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
    SetPendingPresentationTarget(element_id, image_id, lifecycle_.active_image_load_request());
    AcquireLease(EditorOperationLeaseKind::ImageSwitch, current_operation_id_, element_id, image_id,
                 lifecycle_.active_image_load_request(), "Image switch is in progress");
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
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind            = EditorSessionCommandKind::CloseEditor;
    command.persist_changes = persist_changes;
    return SubmitCommand(std::move(command), [this](const EditorSessionCommand& queued) {
      return Close(queued.persist_changes);
    });
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
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind  = EditorSessionCommandKind::PreviewAdjustment;
    command.patch = std::move(patch);
    return SubmitCommand(std::move(command), [this](const EditorSessionCommand& queued) {
      return Patch(queued.patch);
    });
  }
  if (lifecycle_.state() != EditorSessionState::Interactive) {
    return Reject("Patch requires an interactive session");
  }
  if (!lifecycle_.has_image()) {
    return Reject("Patch requires an open image");
  }
  const auto guard   = lifecycle_.history_guard();
  const auto ident   = lifecycle_.identity();
  auto       outcome = edit_.HandlePatch(std::move(patch), false, guard, ident);
  if (outcome.kind == EditorEditOutcome::Kind::Rejected) {
    return Reject(outcome.message);
  }
  const auto route_identity           = lifecycle_.identity();
  const auto load_request             = lifecycle_.active_image_load_request();
  outcome.render_command.operation_id = current_operation_id_;
  render_.RouteInitialRender(outcome.render_command, route_identity, load_request);
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::RenderRouted;
  result.state    = lifecycle_.state();
  result.identity = route_identity;
  result.message  = outcome.message;
  return Emit(std::move(result));
}

auto EditorSessionService::CommitAdjustment(EditorAdjustmentPatch patch) -> EditorSessionResult {
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind  = EditorSessionCommandKind::CommitAdjustment;
    command.patch = std::move(patch);
    return SubmitCommand(std::move(command), [this](const EditorSessionCommand& queued) {
      return CommitAdjustment(queued.patch);
    });
  }
  if (lifecycle_.state() != EditorSessionState::Interactive) {
    return Reject("Patch requires an interactive session");
  }
  if (!lifecycle_.has_image()) {
    return Reject("Patch requires an open image");
  }
  const auto guard   = lifecycle_.history_guard();
  const auto ident   = lifecycle_.identity();
  auto       outcome = edit_.HandlePatch(std::move(patch), true, guard, ident);
  if (outcome.kind == EditorEditOutcome::Kind::Rejected) {
    return Reject(outcome.message);
  }
  const auto route_identity           = lifecycle_.identity();
  const auto load_request             = lifecycle_.active_image_load_request();
  outcome.render_command.operation_id = current_operation_id_;
  render_.RouteInitialRender(outcome.render_command, route_identity, load_request);
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::RenderRouted;
  result.state    = lifecycle_.state();
  result.identity = route_identity;
  result.message  = outcome.message;
  BumpHistoryRevision();
  return Emit(std::move(result));
}

auto EditorSessionService::AddColorGrade(const NodeId& before_node_id, const NodeId& new_id)
    -> EditorSessionResult {
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind           = EditorSessionCommandKind::AddColorGrade;
    command.before_node_id = before_node_id;
    command.node_id        = new_id;
    return SubmitCommand(std::move(command), [this](const EditorSessionCommand& queued) {
      return AddColorGrade(queued.before_node_id, queued.node_id);
    });
  }
  if (lifecycle_.state() != EditorSessionState::Interactive || !dependencies_.history ||
      !lifecycle_.has_history_guard()) {
    return Reject("Color Grade creation requires an interactive history session");
  }
  std::string error;
  if (!dependencies_.history->AddColorGrade(lifecycle_.history_guard(), before_node_id, new_id,
                                            &error)) {
    return Reject(error.empty() ? "Color Grade creation failed" : std::move(error));
  }

  const auto          reason = dependencies_.history->LastPublishedRenderReason();
  EditorSessionResult result;
  result.kind     = reason.has_value() ? EditorSessionResultKind::RenderRouted
                                       : EditorSessionResultKind::Accepted;
  result.state    = lifecycle_.state();
  result.identity = lifecycle_.identity();
  result.message  = "Color Grade created";
  if (reason.has_value()) {
    EditorRenderCommand render_command;
    render_command.reason       = *reason;
    render_command.operation_id = current_operation_id_;
    render_.RouteInitialRender(render_command, result.identity,
                               lifecycle_.active_image_load_request());
  }
  BumpHistoryRevision();
  return Emit(std::move(result));
}

auto EditorSessionService::RemoveColorGrade(const NodeId& node_id) -> EditorSessionResult {
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind    = EditorSessionCommandKind::RemoveColorGrade;
    command.node_id = node_id;
    return SubmitCommand(std::move(command), [this](const EditorSessionCommand& queued) {
      return RemoveColorGrade(queued.node_id);
    });
  }
  if (lifecycle_.state() != EditorSessionState::Interactive || !dependencies_.history ||
      !lifecycle_.has_history_guard()) {
    return Reject("Color Grade removal requires an interactive history session");
  }
  std::string error;
  if (!dependencies_.history->RemoveColorGrade(lifecycle_.history_guard(), node_id, &error)) {
    return Reject(error.empty() ? "Color Grade removal failed" : std::move(error));
  }

  const auto          reason = dependencies_.history->LastPublishedRenderReason();
  EditorSessionResult result;
  result.kind     = reason.has_value() ? EditorSessionResultKind::RenderRouted
                                       : EditorSessionResultKind::Accepted;
  result.state    = lifecycle_.state();
  result.identity = lifecycle_.identity();
  result.message  = "Color Grade removed";
  if (reason.has_value()) {
    EditorRenderCommand render_command;
    render_command.reason       = *reason;
    render_command.operation_id = current_operation_id_;
    render_.RouteInitialRender(render_command, result.identity,
                               lifecycle_.active_image_load_request());
  }
  BumpHistoryRevision();
  return Emit(std::move(result));
}

auto EditorSessionService::RenameColorGrade(const NodeId& node_id, std::string display_name)
    -> EditorSessionResult {
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind    = EditorSessionCommandKind::RenameColorGrade;
    command.node_id = node_id;
    command.text    = std::move(display_name);
    return SubmitCommand(std::move(command), [this](const EditorSessionCommand& queued) {
      return RenameColorGrade(queued.node_id, queued.text);
    });
  }
  if (lifecycle_.state() != EditorSessionState::Interactive || !dependencies_.history ||
      !lifecycle_.has_history_guard()) {
    return Reject("Color Grade rename requires an interactive history session");
  }
  std::string error;
  if (!dependencies_.history->RenameColorGrade(lifecycle_.history_guard(), node_id,
                                               std::move(display_name), &error)) {
    return Reject(error.empty() ? "Color Grade rename failed" : std::move(error));
  }

  const auto          reason = dependencies_.history->LastPublishedRenderReason();
  EditorSessionResult result;
  result.kind     = reason.has_value() ? EditorSessionResultKind::RenderRouted
                                       : EditorSessionResultKind::Accepted;
  result.state    = lifecycle_.state();
  result.identity = lifecycle_.identity();
  result.message  = "Color Grade renamed";
  if (reason.has_value()) {
    EditorRenderCommand render_command;
    render_command.reason       = *reason;
    render_command.operation_id = current_operation_id_;
    render_.RouteInitialRender(render_command, result.identity,
                               lifecycle_.active_image_load_request());
  }
  BumpHistoryRevision();
  return Emit(std::move(result));
}

auto EditorSessionService::ReconnectColorGrade(const NodeId& node_id,
                                               const NodeId& new_predecessor_id,
                                               const NodeId& new_successor_id)
    -> EditorSessionResult {
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind                = EditorSessionCommandKind::ReconnectColorGrade;
    command.node_id             = node_id;
    command.predecessor_node_id = new_predecessor_id;
    command.successor_node_id   = new_successor_id;
    return SubmitCommand(std::move(command), [this](const EditorSessionCommand& queued) {
      return ReconnectColorGrade(queued.node_id, queued.predecessor_node_id,
                                 queued.successor_node_id);
    });
  }
  if (lifecycle_.state() != EditorSessionState::Interactive || !dependencies_.history ||
      !lifecycle_.has_history_guard()) {
    return Reject("Color Grade reconnect requires an interactive history session");
  }
  std::string error;
  if (!dependencies_.history->ReconnectColorGrade(lifecycle_.history_guard(), node_id,
                                                  new_predecessor_id, new_successor_id, &error)) {
    return Reject(error.empty() ? "Color Grade reconnect failed" : std::move(error));
  }

  const auto          reason = dependencies_.history->LastPublishedRenderReason();
  EditorSessionResult result;
  result.kind     = reason.has_value() ? EditorSessionResultKind::RenderRouted
                                       : EditorSessionResultKind::Accepted;
  result.state    = lifecycle_.state();
  result.identity = lifecycle_.identity();
  result.message  = "Color Grade reconnected";
  if (reason.has_value()) {
    EditorRenderCommand render_command;
    render_command.reason       = *reason;
    render_command.operation_id = current_operation_id_;
    render_.RouteInitialRender(render_command, result.identity,
                               lifecycle_.active_image_load_request());
  }
  BumpHistoryRevision();
  return Emit(std::move(result));
}

auto EditorSessionService::EditNodeGraph(NodeGraphTopologyChange change) -> EditorSessionResult {
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind             = EditorSessionCommandKind::EditNodeGraph;
    command.topology_change  = std::move(change);
    return SubmitCommand(std::move(command), [this](const EditorSessionCommand& queued) {
      return EditNodeGraph(queued.topology_change);
    });
  }
  if (lifecycle_.state() != EditorSessionState::Interactive || !dependencies_.history ||
      !lifecycle_.has_history_guard()) {
    return Reject("Node graph topology edit requires an interactive history session");
  }
  std::string error;
  if (!dependencies_.history->EditNodeGraph(lifecycle_.history_guard(), std::move(change),
                                            &error)) {
    return Reject(error.empty() ? "Node graph topology edit failed" : std::move(error));
  }

  const auto          reason = dependencies_.history->LastPublishedRenderReason();
  EditorSessionResult result;
  result.kind     = reason.has_value() ? EditorSessionResultKind::RenderRouted
                                       : EditorSessionResultKind::Accepted;
  result.state    = lifecycle_.state();
  result.identity = lifecycle_.identity();
  result.message  = "Node graph topology updated";
  if (reason.has_value()) {
    EditorRenderCommand render_command;
    render_command.reason       = *reason;
    render_command.operation_id = current_operation_id_;
    render_.RouteInitialRender(render_command, result.identity,
                               lifecycle_.active_image_load_request());
  }
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
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind = EditorSessionCommandKind::Undo;
    return SubmitCommand(std::move(command),
                         [this](const EditorSessionCommand&) { return Undo(); });
  }
  if (lifecycle_.state() != EditorSessionState::Interactive) {
    return Reject("Undo requires interactive state");
  }
  const auto guard   = lifecycle_.history_guard();
  const auto ident   = lifecycle_.identity();
  auto       outcome = edit_.HandleUndoRedo(true, guard, ident);
  if (outcome.kind == EditorEditOutcome::Kind::Rejected) {
    return Reject(outcome.message);
  }
  if (outcome.kind == EditorEditOutcome::Kind::Failed) {
    return Reject(outcome.message);
  }
  const auto undo_identity            = lifecycle_.identity();
  const auto load_request             = lifecycle_.active_image_load_request();
  outcome.render_command.operation_id = current_operation_id_;
  if (outcome.schedule_render) {
    render_.RouteInitialRender(outcome.render_command, undo_identity, load_request);
  }
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::Accepted;
  result.state    = lifecycle_.state();
  result.identity = undo_identity;
  result.message  = outcome.message;
  BumpHistoryRevision();
  return Emit(std::move(result));
}

auto EditorSessionService::Redo() -> EditorSessionResult {
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind = EditorSessionCommandKind::Redo;
    return SubmitCommand(std::move(command),
                         [this](const EditorSessionCommand&) { return Redo(); });
  }
  if (lifecycle_.state() != EditorSessionState::Interactive) {
    return Reject("Redo requires interactive state");
  }
  const auto guard   = lifecycle_.history_guard();
  const auto ident   = lifecycle_.identity();
  auto       outcome = edit_.HandleUndoRedo(false, guard, ident);
  if (outcome.kind == EditorEditOutcome::Kind::Rejected) {
    return Reject(outcome.message);
  }
  if (outcome.kind == EditorEditOutcome::Kind::Failed) {
    return Reject(outcome.message);
  }
  const auto redo_identity            = lifecycle_.identity();
  const auto load_request             = lifecycle_.active_image_load_request();
  outcome.render_command.operation_id = current_operation_id_;
  if (outcome.schedule_render) {
    render_.RouteInitialRender(outcome.render_command, redo_identity, load_request);
  }
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::Accepted;
  result.state    = lifecycle_.state();
  result.identity = redo_identity;
  result.message  = outcome.message;
  BumpHistoryRevision();
  return Emit(std::move(result));
}

auto EditorSessionService::MoveHeadToCommit(const commit_hash_t& commit_id) -> EditorSessionResult {
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind      = EditorSessionCommandKind::MoveHead;
    command.commit_id = commit_id;
    return SubmitCommand(std::move(command), [this](const EditorSessionCommand& queued) {
      return MoveHeadToCommit(queued.commit_id);
    });
  }
  if (lifecycle_.state() != EditorSessionState::Interactive) {
    return Reject("Editor head move requires interactive state");
  }
  const auto guard   = lifecycle_.history_guard();
  const auto ident   = lifecycle_.identity();
  auto       outcome = edit_.HandleMoveHeadToCommit(commit_id, guard, ident);
  if (outcome.kind == EditorEditOutcome::Kind::Rejected) {
    return Reject(outcome.message);
  }
  if (outcome.kind == EditorEditOutcome::Kind::Failed) {
    return Reject(outcome.message);
  }
  const auto move_identity            = lifecycle_.identity();
  const auto load_request             = lifecycle_.active_image_load_request();
  outcome.render_command.operation_id = current_operation_id_;
  if (outcome.schedule_render) {
    render_.RouteInitialRender(outcome.render_command, move_identity, load_request);
  }
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::Accepted;
  result.state    = lifecycle_.state();
  result.identity = move_identity;
  result.message  = outcome.message;
  BumpHistoryRevision();
  return Emit(std::move(result));
}

auto EditorSessionService::Discard() -> EditorSessionResult {
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind = EditorSessionCommandKind::DiscardChanges;
    return SubmitCommand(std::move(command),
                         [this](const EditorSessionCommand&) { return Discard(); });
  }
  const auto state = lifecycle_.state();
  if (state != EditorSessionState::Interactive && state != EditorSessionState::Failed) {
    return Reject("Discard requires an image with an active history session");
  }
  const auto guard   = lifecycle_.history_guard();
  const auto ident   = lifecycle_.identity();
  auto       outcome = edit_.HandleDiscard(guard, ident, state);
  if (outcome.kind == EditorEditOutcome::Kind::Rejected) {
    return Reject(outcome.message);
  }
  if (outcome.kind == EditorEditOutcome::Kind::Failed) {
    return Reject(outcome.message);
  }
  if (state == EditorSessionState::Failed) {
    lifecycle_.BeginRetryFromDiscard();
  }
  const auto discard_identity         = lifecycle_.identity();
  const auto load_request             = lifecycle_.active_image_load_request();
  outcome.render_command.operation_id = current_operation_id_;
  render_.RouteInitialRender(outcome.render_command, discard_identity, load_request);
  EditorSessionResult result;
  result.kind     = (state == EditorSessionState::Failed) ? EditorSessionResultKind::StateChanged
                                                          : EditorSessionResultKind::Accepted;
  result.state    = lifecycle_.state();
  result.identity = lifecycle_.identity();
  result.message  = outcome.message;
  BumpHistoryRevision();
  return Emit(std::move(result));
}

auto EditorSessionService::has_unmaterialized_changes() -> bool {
  if (!lifecycle_.has_image() || !lifecycle_.has_history_guard() || !dependencies_.history) {
    return false;
  }
  std::string error;
  return dependencies_.history->HasUnmaterializedChanges(lifecycle_.history_guard(), &error);
}

auto EditorSessionService::Shutdown() -> EditorSessionResult {
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind = EditorSessionCommandKind::Shutdown;
    return SubmitCommand(std::move(command),
                         [this](const EditorSessionCommand&) { return Shutdown(); });
  }
  if (lifecycle_.state() == EditorSessionState::ShuttingDown) {
    return Reject("Already shutting down");
  }
  // Cancel outstanding save work. Each in-flight checkpoint publishes one
  // terminal cancellation through the posted completion path; navigation keeps
  // image A on that failure path and clears the pending action.
  save_service_.CancelAndWait();
  navigation_.ClearPendingAction();
  lifecycle_.BeginShutdown();
  // The queue stops admitting user commands; already-posted worker completions
  // still reduce so in-flight saves reach a terminal outcome.
  command_queue_.BeginShutdown();
  EditorSessionResult result;
  result.kind     = EditorSessionResultKind::StateChanged;
  result.state    = lifecycle_.state();
  result.identity = lifecycle_.identity();
  result.message  = "Shutting down";
  BumpHistoryRevision();
  return Emit(std::move(result));
}

void EditorSessionService::SetPresentationSinkId(PresentationSinkId sink_id) {
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind                 = EditorSessionCommandKind::SetPresentationTarget;
    command.presentation_sink_id = sink_id;
    (void)SubmitCommand(std::move(command), [this](const EditorSessionCommand& queued) {
      SetPresentationSinkId(queued.presentation_sink_id);
      EditorSessionResult accepted;
      accepted.kind     = EditorSessionResultKind::Accepted;
      accepted.state    = lifecycle_.state();
      accepted.identity = lifecycle_.identity();
      accepted.message  = "Presentation sink updated";
      // Presentation binding is not a history/operation terminal; notify only.
      NotifyChange();
      return accepted;
    });
    return;
  }
  render_.SetPresentationSinkId(sink_id);
  NotifyChange();
}

void EditorSessionService::SetPresentationSize(int width, int height) {
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind                = EditorSessionCommandKind::SetPresentationSize;
    command.presentation_width  = width;
    command.presentation_height = height;
    (void)SubmitCommand(std::move(command), [this](const EditorSessionCommand& queued) {
      SetPresentationSize(queued.presentation_width, queued.presentation_height);
      EditorSessionResult accepted;
      accepted.kind     = EditorSessionResultKind::Accepted;
      accepted.state    = lifecycle_.state();
      accepted.identity = lifecycle_.identity();
      accepted.message  = "Presentation size updated";
      NotifyChange();
      return accepted;
    });
    return;
  }
  render_.SetPresentationSize(width, height);
  NotifyChange();
}

void EditorSessionService::SetGeometryOverlayActive(bool active) {
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind                    = EditorSessionCommandKind::SetGeometryOverlay;
    command.geometry_overlay_active = active;
    (void)SubmitCommand(std::move(command), [this](const EditorSessionCommand& queued) {
      SetGeometryOverlayActive(queued.geometry_overlay_active);
      EditorSessionResult accepted;
      accepted.kind     = EditorSessionResultKind::Accepted;
      accepted.state    = lifecycle_.state();
      accepted.identity = lifecycle_.identity();
      accepted.message  = "Geometry overlay updated";
      return accepted;
    });
    return;
  }
  render_.SetGeometryOverlayActive(active);
}

auto EditorSessionService::RequestViewChange(EditorRenderReason                  reason,
                                             std::optional<ViewportRenderRegion> region)
    -> EditorSessionResult {
  if (!reducing_command_) {
    EditorSessionCommand command;
    command.kind        = EditorSessionCommandKind::RequestViewChange;
    command.view_reason = reason;
    command.view_region = std::move(region);
    return SubmitCommand(std::move(command), [this](const EditorSessionCommand& queued) {
      return RequestViewChange(queued.view_reason, queued.view_region);
    });
  }
  EditorRenderCommand command;
  command.operation_id = current_operation_id_;
  command.reason       = reason;
  command.view_region  = std::move(region);
  const auto view_identity  = lifecycle_.identity();
  const auto view_state     = lifecycle_.state();
  const auto load_request   = lifecycle_.active_image_load_request();
  const auto event =
      render_.RouteViewChange(command, view_identity, load_request, view_state);
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

void EditorSessionService::NotifyImageAcquired(ImageLoadRequestId image_load_request, bool success,
                                               std::string message) {
  EditorSessionCompletion completion;
  completion.kind               = EditorSessionCompletionKind::ImageStateLoaded;
  completion.image_load_request = image_load_request;
  completion.success            = success;
  completion.message            = std::move(message);
  PostCompletion(std::move(completion));
}

void EditorSessionService::NotifyRenderResult(const EditorRenderResult& render_result) {
  EditorSessionCompletion completion;
  completion.kind                 = EditorSessionCompletionKind::RenderResult;
  completion.operation.command_id = render_result.intent.operation_id;
  completion.request_id           = render_result.request_id;
  completion.render_result        = render_result;
  command_queue_.PostCompletion([this, completion = std::move(completion)]() mutable {
    const auto previous_operation = current_operation_id_;
    const auto previous_reducing  = reducing_command_;
    current_operation_id_         = completion.operation.command_id;
    navigation_.SetOperationId(current_operation_id_);
    reducing_command_ = true;
    BeginPublication();
    render_.NotifyRenderResult(completion.render_result, lifecycle_.identity(),
                               lifecycle_.active_image_load_request(), lifecycle_.state());
    EndPublication();
    reducing_command_     = previous_reducing;
    current_operation_id_ = previous_operation;
  });
}

void EditorSessionService::SetActionAvailabilityObserver(ActionAvailabilityObserver observer) {
  action_availability_observer_ = std::move(observer);
}

void EditorSessionService::SetCopiedPackageAvailable(bool available) {
  package_available_ = available;
  PublishActionAvailabilityIfChanged();
}

void EditorSessionService::SetBackgroundActionRestrictions(
    const EditorBackgroundActionRestrictions& restrictions) {
  background_restrictions_ = restrictions;
  PublishActionAvailabilityIfChanged();
}

auto EditorSessionService::BuildActionInputs() -> EditorActionInputs {
  EditorActionInputs inputs;
  inputs.session_state = lifecycle_.state();
  inputs.has_image     = lifecycle_.has_image();
  inputs.package_available = package_available_;
  inputs.background_blocks_select_image = background_restrictions_.blocks_select_image;
  inputs.background_blocks_paste        = background_restrictions_.blocks_paste;
  inputs.background_blocks_checkout     = background_restrictions_.blocks_checkout;
  inputs.background_blocks_workspace    = background_restrictions_.blocks_workspace;
  // A second selection may queue/replace while a switch save or acquire is
  // already in flight (CQ1 pending_next_target). Allow SelectImage as soon as
  // navigation owns a pending action, not only after a target was queued.
  inputs.can_replace_unstarted_selection =
      navigation_state_.pending_action.has_value() ||
      navigation_state_.pending_next_target.has_value();
  inputs.recovery_allows_retry =
      navigation_.has_pending_recovery() &&
      lifecycle_.state() == EditorSessionState::RetainedImageFailure;
  inputs.recovery_allows_discard_continue = inputs.recovery_allows_retry;
  inputs.recovery_allows_cancel           = inputs.recovery_allows_retry;

  if (dependencies_.history && lifecycle_.has_history_guard()) {
    std::string error;
  EditorHistorySnapshot snapshot;
    if (dependencies_.history->ReadHistorySnapshot(lifecycle_.history_guard(), &snapshot, &error)) {
      inputs.can_undo = snapshot.can_undo;
      inputs.can_redo = snapshot.can_redo;
    }
  }
  inputs.has_unmaterialized_changes = has_unmaterialized_changes();
  return inputs;
}

void EditorSessionService::PublishActionAvailabilityIfChanged() {
  const auto next =
      EditorActionPolicy::EvaluateAll(EditorCommandContext{active_leases_}, BuildActionInputs());
  if (next == published_availability_) {
    return;
  }
  published_availability_ = next;
  if (action_availability_observer_) {
    action_availability_observer_(published_availability_);
  }
}

void EditorSessionService::AcquireLease(EditorOperationLeaseKind kind, std::uint64_t command_id,
                                        sl_element_id_t element_id, image_id_t image_id,
                                        ImageLoadRequestId image_load_request,
                                        std::string blocking_reason) {
  EditorOperationLease lease;
  lease.operation.command_id = command_id;
  lease.kind                 = kind;
  lease.target_element_id    = element_id;
  lease.target_image_id      = image_id;
  lease.image_load_request   = image_load_request;
  lease.blocked_actions      = EditorActionPolicy::DefaultBlockedActions(kind);
  lease.blocking_reason      = std::move(blocking_reason);
  active_leases_.push_back(std::move(lease));
}

void EditorSessionService::ReleaseLeaseByCommandId(std::uint64_t command_id) {
  if (command_id == 0) {
    return;
  }
  std::erase_if(active_leases_, [command_id](const EditorOperationLease& lease) {
    return lease.operation.command_id == command_id;
  });
}

void EditorSessionService::ReleaseLeasesByKind(EditorOperationLeaseKind kind) {
  std::erase_if(active_leases_, [kind](const EditorOperationLease& lease) {
    return lease.kind == kind;
  });
}

void EditorSessionService::SetPendingPresentationTarget(sl_element_id_t element_id,
                                                      image_id_t       image_id,
                                                      ImageLoadRequestId image_load_request) {
  pending_presentation_target_ =
      EditorPendingPresentationTarget{element_id, image_id, image_load_request};
}

void EditorSessionService::ClearPendingPresentationTarget() {
  pending_presentation_target_.reset();
}

}  // namespace alcedo
