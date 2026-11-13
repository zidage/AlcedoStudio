//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/automation/automation_editor_operation.hpp"

#include <utility>

#include "ui/alcedo_main/album_backend/editor_session_controller.hpp"

namespace alcedo::automation {

auto OperationWait::Begin(ui::EditorSessionController* editor, QObject* context, OperationDone done,
                          OperationEnd end) -> std::shared_ptr<OperationWait> {
  auto wait         = std::shared_ptr<OperationWait>(new OperationWait(std::move(done), end));
  wait->self_       = wait;
  wait->connection_ = QObject::connect(
      editor, &ui::EditorSessionController::SessionResultObserved, context,
      [weak = std::weak_ptr<OperationWait>(wait)](const EditorSessionResult& result) {
        if (auto locked = weak.lock()) {
          locked->Observe(result);
        }
      });
  // A wait whose reply timed out never finishes; it ends with its context.
  QObject::connect(context, &QObject::destroyed, [weak = std::weak_ptr<OperationWait>(wait)] {
    if (auto locked = weak.lock()) {
      locked->self_.reset();
    }
  });
  return wait;
}

OperationWait::OperationWait(OperationDone done, OperationEnd end)
    : done_(std::move(done)), end_(end) {}

void OperationWait::Start(std::uint64_t operation_id) {
  if (finished_) {
    return;
  }
  if (operation_id == 0) {
    Finish(OperationFailure{AutomationErrorCode::Failed,
                            QStringLiteral("The editor did not assign a command id.")});
    return;
  }
  operation_id_ = operation_id;
  auto early    = std::exchange(early_, {});
  for (const auto& result : early) {
    Observe(result);
  }
}

void OperationWait::Fail(OperationFailure failure) { Finish(std::move(failure)); }

void OperationWait::Observe(const EditorSessionResult& result) {
  if (finished_) {
    return;
  }
  if (operation_id_ == 0) {
    early_.push_back(result);
    return;
  }
  if (result.kind == EditorSessionResultKind::FramePresented ||
      result.kind == EditorSessionResultKind::FrameDeferred ||
      result.kind == EditorSessionResultKind::FrameFailed) {
    if (render_request_id_ == 0 || result.render_request_id < render_request_id_) {
      return;
    }
    // A minimized window defers the frame; the change itself is applied and is presented when
    // the window is shown, so the command succeeds instead of waiting for the user.
    if (result.kind == EditorSessionResultKind::FramePresented ||
        result.kind == EditorSessionResultKind::FrameDeferred) {
      Finish(std::nullopt);
      return;
    }
    Finish(OperationFailure{
        AutomationErrorCode::Failed,
        QStringLiteral("The change is applied, but its frame failed to render: %1")
            .arg(QString::fromStdString(result.message)),
        /*committed_=*/true});
    return;
  }
  if (result.operation_id != operation_id_) {
    return;
  }
  if (EditorSessionResultIsFailure(result.kind)) {
    Finish(OperationFailure{result.kind == EditorSessionResultKind::Rejected
                                ? AutomationErrorCode::Rejected
                                : AutomationErrorCode::Failed,
                            QString::fromStdString(result.message)});
    return;
  }
  if (end_ == OperationEnd::AfterSaveCheckpoint &&
      result.kind == EditorSessionResultKind::SaveFinished) {
    return;
  }
  if (result.render_request_id != 0) {
    render_request_id_ = result.render_request_id;
    return;
  }
  if (render_request_id_ == 0 && EditorSessionResultIsTerminal(result.kind)) {
    Finish(std::nullopt);
  }
}

void OperationWait::Finish(std::optional<OperationFailure> failure) {
  if (finished_) {
    return;
  }
  finished_ = true;
  QObject::disconnect(connection_);
  auto done = std::move(done_);
  auto self = std::move(self_);
  if (done) {
    done(std::move(failure));
  }
}

void AfterQueuedChanges(QObject* context, std::function<void()> task) {
  QMetaObject::invokeMethod(context, std::move(task), Qt::QueuedConnection);
}

}  // namespace alcedo::automation
