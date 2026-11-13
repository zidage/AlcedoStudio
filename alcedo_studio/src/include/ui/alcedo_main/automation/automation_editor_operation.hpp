//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QMetaObject>
#include <QObject>
#include <QString>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "app/editor_session_types.hpp"
#include "automation/automation_protocol.hpp"

namespace alcedo::ui {
class EditorSessionController;
}  // namespace alcedo::ui

namespace alcedo::automation {

/// A failed session command: the error code of the response and the owner message.
struct OperationFailure {
  AutomationErrorCode code_ = AutomationErrorCode::Failed;
  QString             message_;
  /// True when the command committed its change and only the render of that commit failed: the
  /// change stays in the history.
  bool                committed_ = false;
};

using OperationDone = std::function<void(std::optional<OperationFailure>)>;

/// The result that ends a session command.
enum class OperationEnd : std::uint8_t {
  /// The first terminal result.
  FirstTerminal,
  /// The first terminal result that is not SaveFinished. A Version command saves a checkpoint
  /// first; its SaveFinished comes before the result of the Version change.
  AfterSaveCheckpoint,
};

/**
 * @brief Waits for the results of one session command.
 *
 * The command is done after its terminal result (see OperationEnd). When one of its results
 * routed a render, it is done after a presented frame of that render request or a later one (a
 * later request renders the later document, which includes the change). A failed render of
 * that request or a later one fails it with a committed failure. A Rejected or Failed result of
 * the command fails it. Results that arrive before the command id is known are kept
 * and checked once it is.
 */
class OperationWait final {
 public:
  /// Connects to @p editor with @p context as the connection context. Call Start with the id
  /// that the submission returned.
  static auto Begin(ui::EditorSessionController* editor, QObject* context, OperationDone done,
                    OperationEnd end = OperationEnd::FirstTerminal)
      -> std::shared_ptr<OperationWait>;

  void Start(std::uint64_t operation_id);
  void Fail(OperationFailure failure);

 private:
  OperationWait(OperationDone done, OperationEnd end);

  void                             Observe(const EditorSessionResult& result);
  void                             Finish(std::optional<OperationFailure> failure);

  OperationDone                    done_;
  OperationEnd                     end_;
  QMetaObject::Connection          connection_;
  std::vector<EditorSessionResult> early_;
  std::uint64_t                    operation_id_      = 0;
  std::uint64_t                    render_request_id_ = 0;
  bool                             finished_          = false;
  /// Keeps the wait alive until it finishes; the connection holds only a weak reference.
  std::shared_ptr<OperationWait>   self_;
};

/// Runs @p task on the event loop of @p context after the change notifications that are queued
/// now: the session publishes a result before the change that updates the snapshots.
void AfterQueuedChanges(QObject* context, std::function<void()> task);

}  // namespace alcedo::automation
