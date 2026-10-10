//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/application_close_coordinator.hpp"

#include <QMetaObject>

#include "app/editor_session_types.hpp"
#include "ui/alcedo_main/album_backend/editor_action_availability_model.hpp"

namespace alcedo::ui {
namespace {

auto IsPersistBusy(const EditorSessionController& session) -> bool {
  const auto state = session.session_state();
  return session.close_in_flight() || session.persist_in_flight() ||
         state == alcedo::EditorSessionState::Saving ||
         state == alcedo::EditorSessionState::Switching;
}

}  // namespace

ApplicationCloseCoordinator::ApplicationCloseCoordinator(EditorSessionController* editor_session,
                                                         WorkspaceRouter*         workspace_router,
                                                         ProjectModule* project, QObject* parent)
    : QObject(parent),
      editor_session_(editor_session),
      workspace_router_(workspace_router),
      project_(project) {
  connect(this, &ApplicationCloseCoordinator::ApplicationCloseFinished, this,
          &ApplicationCloseCoordinator::applicationCloseFinished);
  connect(this, &ApplicationCloseCoordinator::ApplicationCloseAborted, this,
          &ApplicationCloseCoordinator::applicationCloseAborted);
  if (editor_session_ != nullptr) {
    // Queued: the evaluation reads the session after the change that emitted the signal, and a
    // finalize that changes state synchronously does not finish the close inside the Begin call.
    connect(editor_session_, &EditorSessionController::StateChanged, this,
            &ApplicationCloseCoordinator::EvaluateEditorClose, Qt::QueuedConnection);
  }
}

bool ApplicationCloseCoordinator::CloseNeedsConfirmation() const {
  if (waiting()) {
    return true;
  }
  if (editor_session_ == nullptr) {
    return false;
  }
  const bool persist_busy = IsPersistBusy(*editor_session_);
  if (workspace_router_ == nullptr || workspace_router_->workspace() != QStringLiteral("editor")) {
    return persist_busy;
  }
  return persist_busy || editor_session_->has_image();
}

bool ApplicationCloseCoordinator::EditorPersistBusy() const {
  return editor_session_ != nullptr && IsPersistBusy(*editor_session_);
}

void ApplicationCloseCoordinator::BeginSave() {
  if (waiting()) {
    return;
  }
  // Application exit seals the editor session. Ordinary workspace routing keeps it alive.
  Start(Mode::kApplicationExit, /*awaiting_seal=*/true);
  if (editor_session_ != nullptr) {
    editor_session_->Finalize(true);
  }
  if (workspace_router_ != nullptr) {
    workspace_router_->OpenLibrary();
  }
  ScheduleEvaluation();
}

void ApplicationCloseCoordinator::BeginDiscard() {
  if (waiting()) {
    return;
  }
  if (editor_session_ == nullptr) {
    emit ApplicationCloseFinished();
    return;
  }
  if (editor_session_->has_pending_recovery() &&
      editor_session_->actions()->can_discard_and_continue()) {
    editor_session_->DiscardAndContinue();
    emit ApplicationCloseFinished();
    return;
  }
  Start(Mode::kApplicationExit, /*awaiting_seal=*/true);
  editor_session_->Finalize(false);
  if (workspace_router_ != nullptr) {
    workspace_router_->OpenLibrary();
  }
  ScheduleEvaluation();
}

void ApplicationCloseCoordinator::BeginPersistWait() {
  if (waiting()) {
    return;
  }
  Start(Mode::kApplicationExit, /*awaiting_seal=*/false);
  ScheduleEvaluation();
}

void ApplicationCloseCoordinator::Cancel() {
  if (!waiting()) {
    return;
  }
  mode_          = Mode::kIdle;
  awaiting_seal_ = false;
  emit WaitingChanged();
}

auto ApplicationCloseCoordinator::BeginProjectClose(bool persist) -> QString {
  if (waiting()) {
    return tr("Another close is in progress.");
  }
  if (project_ == nullptr) {
    return tr("No project is loaded yet.");
  }
  // The editor image is finalized below, so only the project rules decide here.
  if (const QString reason = project_->ProjectCloseBlockReason(); !reason.isEmpty()) {
    return reason;
  }
  project_close_persist_ = persist;
  const bool finalize_editor =
      editor_session_ != nullptr && (editor_session_->active() || editor_session_->has_image());
  Start(Mode::kProjectClose, /*awaiting_seal=*/finalize_editor);
  if (finalize_editor) {
    editor_session_->Finalize(persist);
  }
  ScheduleEvaluation();
  return {};
}

void ApplicationCloseCoordinator::Start(Mode mode, bool awaiting_seal) {
  mode_          = mode;
  awaiting_seal_ = awaiting_seal;
  emit WaitingChanged();
}

void ApplicationCloseCoordinator::ScheduleEvaluation() {
  QMetaObject::invokeMethod(this, &ApplicationCloseCoordinator::EvaluateEditorClose,
                            Qt::QueuedConnection);
}

void ApplicationCloseCoordinator::EvaluateEditorClose() {
  if (!waiting()) {
    return;
  }
  if (editor_session_ == nullptr) {
    Finish();
    return;
  }
  const auto state = editor_session_->session_state();
  // A queued owner-thread close or persist still reports Interactive until the reducer runs, so
  // the in-flight flags count as well as Saving and Switching.
  if (IsPersistBusy(*editor_session_)) {
    return;
  }
  if (editor_session_->has_pending_recovery() ||
      state == alcedo::EditorSessionState::RetainedImageFailure ||
      state == alcedo::EditorSessionState::Failed) {
    Abort(editor_session_->last_error());
    return;
  }
  if (!awaiting_seal_ || state == alcedo::EditorSessionState::NoImage ||
      state == alcedo::EditorSessionState::ShuttingDown) {
    Finish();
    return;
  }
  // The seal was rejected and the image is still open: finishing would drop unsaved work.
  Abort(editor_session_->last_error());
}

void ApplicationCloseCoordinator::Finish() {
  const Mode mode = mode_;
  mode_           = Mode::kIdle;
  awaiting_seal_  = false;
  emit WaitingChanged();
  if (mode == Mode::kApplicationExit) {
    emit ApplicationCloseFinished();
    return;
  }
  if (project_ == nullptr) {
    emit ProjectCloseFinished(false, tr("No project is loaded yet."));
    return;
  }
  if (!project_->CloseProject(project_close_persist_)) {
    emit ProjectCloseFinished(false, project_->ServiceMessage());
    return;
  }
  emit ProjectCloseFinished(true, QString());
}

void ApplicationCloseCoordinator::Abort(const QString& message) {
  const Mode mode = mode_;
  mode_           = Mode::kIdle;
  awaiting_seal_  = false;
  emit WaitingChanged();
  if (mode == Mode::kApplicationExit) {
    emit ApplicationCloseAborted(message);
    return;
  }
  emit ProjectCloseFinished(false,
                            message.isEmpty() ? tr("The editor image cannot be saved.") : message);
}

}  // namespace alcedo::ui
