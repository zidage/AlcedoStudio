//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_history_operation_publisher.hpp"

namespace alcedo::ui {
namespace {

auto StageFor(const alcedo::EditorSessionResult& result, bool terminal)
    -> alcedo::HistoryOperationStage {
  if (terminal) {
    return alcedo::HistoryOperationStage::Completed;
  }
  if (result.kind == alcedo::EditorSessionResultKind::SaveStarted) {
    return alcedo::HistoryOperationStage::Saving;
  }
  return alcedo::HistoryOperationStage::Requested;
}

}  // namespace

auto EditorHistoryOperationPublisher::AllocateOperationId() -> std::uint64_t {
  return next_operation_id_++;
}

auto EditorHistoryOperationPublisher::BuildEvent(std::uint64_t operation_id, const QString& action,
                                                 const alcedo::EditorSessionResult& result,
                                                 bool terminal, const QString& selected_id)
    -> HistoryOperationEvent {
  HistoryOperationEvent event;
  event.operation_id       = operation_id;
  event.action             = action.toStdString();
  event.stage              = StageFor(result, terminal);
  event.terminal           = terminal;
  event.result_kind        = result.kind;
  event.state              = result.state;
  event.element_id         = result.identity.element_id;
  event.image_id           = result.identity.image_id;
  event.checkpoint_task_id = result.task_id;
  event.render_request_id  = result.render_request_id;
  event.selected_id        = selected_id.toStdString();
  event.message            = result.message;
  return event;
}

auto EditorHistoryOperationPublisher::ToVariantMap(const HistoryOperationEvent& event)
    -> QVariantMap {
  QVariantMap map;
  map.insert(QStringLiteral("operationId"), static_cast<qulonglong>(event.operation_id));
  map.insert(QStringLiteral("action"), QString::fromStdString(event.action));
  map.insert(QStringLiteral("stage"), static_cast<int>(event.stage));
  map.insert(QStringLiteral("terminal"), event.terminal);
  map.insert(QStringLiteral("kind"), static_cast<int>(event.result_kind));
  map.insert(QStringLiteral("state"),
             QString::fromStdString(alcedo::EditorSessionStateName(event.state)));
  map.insert(QStringLiteral("taskId"), static_cast<qulonglong>(event.checkpoint_task_id));
  map.insert(QStringLiteral("renderRequestId"), static_cast<qulonglong>(event.render_request_id));
  map.insert(QStringLiteral("elementId"), static_cast<uint>(event.element_id));
  map.insert(QStringLiteral("imageId"), static_cast<uint>(event.image_id));
  map.insert(QStringLiteral("selectedId"), QString::fromStdString(event.selected_id));
  map.insert(QStringLiteral("message"), QString::fromStdString(event.message));
  return map;
}

auto EditorHistoryOperationPublisher::Store(Published published) -> Published {
  last_ = std::move(published);
  return last_;
}

auto EditorHistoryOperationPublisher::PublishRejected(std::uint64_t operation_id,
                                                      const QString& action,
                                                      const QString& message,
                                                      const QString& selected_id) -> Published {
  alcedo::EditorSessionResult result;
  result.kind    = alcedo::EditorSessionResultKind::Rejected;
  result.message = message.toStdString();
  pending_.reset();
  Published published;
  published.event   = BuildEvent(operation_id, action, result, /*terminal=*/true, selected_id);
  published.map     = ToVariantMap(published.event);
  published.message = message;
  published.failed  = true;
  return Store(std::move(published));
}

auto EditorHistoryOperationPublisher::PublishInvokableReturn(
    std::uint64_t operation_id, const QString& action, const alcedo::EditorSessionResult& result,
    const QString& selected_id) -> Published {
  const bool terminal = alcedo::EditorSessionResultIsTerminal(result.kind);
  if (terminal) {
    pending_.reset();
  } else {
    Pending pending;
    pending.operation_id = operation_id;
    pending.action       = action;
    pending.selected_id  = selected_id;
    pending.task_id      = result.task_id;
    pending_             = std::move(pending);
  }
  Published published;
  published.event   = BuildEvent(operation_id, action, result, terminal, selected_id);
  published.map     = ToVariantMap(published.event);
  published.message = QString::fromStdString(result.message);
  published.failed  = alcedo::EditorSessionResultIsFailure(result.kind);
  return Store(std::move(published));
}

auto EditorHistoryOperationPublisher::CorrelateObservedResult(
    const alcedo::EditorSessionResult& result) -> std::optional<Published> {
  if (!pending_.has_value()) {
    return std::nullopt;
  }
  if (result.kind == alcedo::EditorSessionResultKind::SaveStarted) {
    // The invokable path already published the start event. Ignore duplicates
    // emitted by the service during the same call or a late re-notify.
    if (pending_->task_id == 0 && result.task_id != 0) {
      pending_->task_id = result.task_id;
    }
    return std::nullopt;
  }
  if (!alcedo::EditorSessionResultIsTerminal(result.kind)) {
    return std::nullopt;
  }
  // Pending Version/create/branch/checkout ops always carry the save checkpoint
  // task id. Ignore terminal noise without that id (e.g. intermediate
  // RenderRouted from RouteInitialRender inside Continue*) so the draft stays
  // correlated until NotifyCompletion publishes the checkpoint ticket.
  if (pending_->task_id != 0) {
    if (result.task_id == 0) {
      return std::nullopt;
    }
    if (pending_->task_id != result.task_id) {
      // Stale completion for a different checkpoint ticket.
      return std::nullopt;
    }
  }

  const auto operation_id = pending_->operation_id;
  const auto action       = pending_->action;
  const auto selected_id  = pending_->selected_id;
  pending_.reset();

  Published published;
  published.event   = BuildEvent(operation_id, action, result, /*terminal=*/true, selected_id);
  published.map     = ToVariantMap(published.event);
  published.message = QString::fromStdString(result.message);
  published.failed  = alcedo::EditorSessionResultIsFailure(result.kind);
  return Store(std::move(published));
}

}  // namespace alcedo::ui
