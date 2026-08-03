//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QMetaObject>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariant>
#include <optional>
#include <utility>

namespace alcedo::ui {

class ProbeItemTree;

/// Non-blocking property wait for the test probe.
///
/// Owns the single in-flight wait state (timers, notify connection, last
/// observed value). Never blocks the GUI thread: uses the property notify
/// signal when available and a poll timer otherwise. Emits @c ReplyReady with
/// the completed JSON Lines response.
class ProbePropertyWait final : public QObject {
  Q_OBJECT

 public:
  explicit ProbePropertyWait(ProbeItemTree* tree, QObject* parent = nullptr);
  ~ProbePropertyWait() override;

  /// Starts an async wait for @p request. Emits @c ReplyReady on match or timeout.
  /// If another wait is active, emits an immediate wait_busy error reply.
  void Begin(const QJsonObject& request);

  /// Cancels any in-flight wait without emitting a reply.
  void Cancel();

  [[nodiscard]] bool busy() const { return pending_.has_value(); }

 signals:
  void ReplyReady(const QJsonObject& response);

 private slots:
  void Evaluate();
  void FinishTimeout();

 private:
  struct PendingWait {
    QJsonValue              request_id;
    QString                 target;
    QString                 property_name;
    QString                 matcher;
    QJsonValue              expected;
    QVariant                last_actual;
    QMetaObject::Connection notify_connection;
    QTimer*                 poll_timer    = nullptr;
    QTimer*                 timeout_timer = nullptr;
  };

  void ClearPending(bool emit_reply, bool matched);
  void SubscribeNotifyIfAvailable();

  [[nodiscard]] static bool MatcherHolds(const QString& matcher, const QVariant& actual,
                                         const QJsonValue& expected);
  [[nodiscard]] static auto ParseMatcher(const QJsonObject& request)
      -> std::pair<QString, QJsonValue>;

  ProbeItemTree*             tree_ = nullptr;
  std::optional<PendingWait> pending_;
};

}  // namespace alcedo::ui
