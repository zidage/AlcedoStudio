//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include "probe_input_injector.hpp"
#include "probe_item_tree.hpp"
#include "probe_property_wait.hpp"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalServer>
#include <QLocalSocket>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>

class QQmlApplicationEngine;
class QQuickWindow;

namespace alcedo::ui {

/// GUI-thread JSON Lines probe facade for the automation test host.
///
/// Owns the QLocalServer transport, heartbeats, and ready events. Observation,
/// input injection, and property waits are delegated to @c ProbeItemTree,
/// @c ProbeInputInjector, and @c ProbePropertyWait respectively.
class TestProbe final : public QObject {
  Q_OBJECT

 public:
  explicit TestProbe(QQmlApplicationEngine* engine, QQuickWindow* window,
                     QObject* parent = nullptr);
  ~TestProbe() override = default;

  /// Starts the local server. @p socketName must be non-empty.
  /// @return false on listen failure; writes @p error when non-null.
  bool Start(const QString& socketName, QString* error = nullptr);

  /// Emits the unsolicited `ready` event once project/import startup has settled.
  void MarkReady();

  [[nodiscard]] bool    ready() const { return ready_; }
  [[nodiscard]] QString socket_name() const { return socket_name_; }

 private slots:
  void HandleNewConnection();
  void HandleReadyRead();
  void HandleSocketDisconnected();
  void EmitHeartbeat();
  void ForwardWaitReply(const QJsonObject& response);

 private:
  [[nodiscard]] auto HandleRequest(const QJsonObject& request) -> QJsonObject;

  void SendJson(const QJsonObject& object);
  void SendReadyEvent();
  void SendError(const QJsonValue& request_id, const QString& code, const QString& message,
                  const QJsonObject& extra = {});

  QPointer<QQuickWindow> window_;
  ProbeItemTree          tree_;
  ProbeInputInjector     input_;
  ProbePropertyWait      wait_;
  QLocalServer           server_;
  QPointer<QLocalSocket> client_;
  QTimer                 heartbeat_timer_;
  QElapsedTimer          elapsed_timer_;
  QString                socket_name_;
  quint64                heartbeat_counter_ = 0;
  bool                   ready_             = false;
};

}  // namespace alcedo::ui
