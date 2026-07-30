//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <QString>
#include <QVariantMap>

#include "app/editor_session_types.hpp"

namespace alcedo::ui {

/// Correlates one user-initiated history/Version action with its start and
/// terminal events at the QML boundary.
///
/// Owns operation-id allocation, pending-async correlation (action + optional
/// checkpoint task id), and the last published event map. Does not own the
/// session backend, pipeline, or QObject lifetime — `EditorSessionController`
/// installs the result observer and emits QML signals after each publication.
///
/// Thread affinity: call only on the GUI thread that owns the controller.
class EditorHistoryOperationPublisher final {
 public:
  /// Presentation of one published event for QML properties and signals.
  struct Published {
    HistoryOperationEvent event;
    QVariantMap           map;
    QString               message;
    bool                  failed = false;
  };

  EditorHistoryOperationPublisher() = default;

  /// Allocate a new operation id for the next user action. Does not publish.
  [[nodiscard]] auto AllocateOperationId() -> std::uint64_t;

  /// Build a rejected terminal event without calling the backend (invalid id,
  /// empty name, missing backend). Marks the operation complete.
  [[nodiscard]] auto PublishRejected(std::uint64_t operation_id, const QString& action,
                                     const QString& message,
                                     const QString& selected_id = {}) -> Published;

  /// Publish the invokable return value. `SaveStarted` becomes a non-terminal
  /// pending operation; other kinds become the single terminal event.
  ///
  /// Preconditions: `operation_id` was allocated for this action. Clears any
  /// previous pending state for a new terminal, or replaces pending on start.
  [[nodiscard]] auto PublishInvokableReturn(std::uint64_t operation_id, const QString& action,
                                            const alcedo::EditorSessionResult& result,
                                            const QString& selected_id = {}) -> Published;

  /// Correlate an asynchronous backend result with the pending operation.
  ///
  /// Returns nullopt when there is no pending operation, the result is another
  /// `SaveStarted`, the checkpoint task id does not match a known pending task,
  /// or a terminal event was already published for the pending id. On success
  /// publishes exactly one terminal event and clears pending.
  [[nodiscard]] auto CorrelateObservedResult(const alcedo::EditorSessionResult& result)
      -> std::optional<Published>;

  [[nodiscard]] auto last_published() const -> const Published& { return last_; }
  [[nodiscard]] auto has_pending() const -> bool { return pending_.has_value(); }
  [[nodiscard]] auto pending_operation_id() const -> std::uint64_t {
    return pending_ ? pending_->operation_id : 0;
  }
  [[nodiscard]] auto pending_action() const -> QString {
    return pending_ ? pending_->action : QString{};
  }

 private:
  struct Pending {
    std::uint64_t operation_id = 0;
    QString       action;
    QString       selected_id;
    std::uint64_t task_id = 0;
  };

  [[nodiscard]] static auto BuildEvent(std::uint64_t operation_id, const QString& action,
                                       const alcedo::EditorSessionResult& result, bool terminal,
                                       const QString& selected_id) -> HistoryOperationEvent;
  [[nodiscard]] static auto ToVariantMap(const HistoryOperationEvent& event) -> QVariantMap;
  [[nodiscard]] auto        Store(Published published) -> Published;

  std::uint64_t         next_operation_id_ = 1;
  std::optional<Pending> pending_;
  Published             last_;
};

}  // namespace alcedo::ui
