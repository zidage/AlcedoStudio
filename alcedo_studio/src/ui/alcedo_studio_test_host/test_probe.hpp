//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QElapsedTimer>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocalServer>
#include <QLocalSocket>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <optional>

class QQmlApplicationEngine;
class QQuickItem;
class QQuickWindow;

namespace alcedo::ui {

/// GUI-thread JSON Lines probe for deterministic inspection of the test host.
class TestProbe final : public QObject {
  Q_OBJECT

 public:
  explicit TestProbe(QQmlApplicationEngine* engine, QQuickWindow* window,
                     QObject* parent = nullptr);
  ~TestProbe() override = default;

  bool                  Start(const QString& socketName, QString* error = nullptr);
  void                  MarkReady();

  [[nodiscard]] bool    ready() const { return ready_; }
  [[nodiscard]] QString socket_name() const { return socket_name_; }

 private slots:
  void HandleNewConnection();
  void HandleReadyRead();
  void HandleSocketDisconnected();
  void EmitHeartbeat();

 private:
  struct TargetMatch {
    QQuickItem* item = nullptr;
    QStringList path;
  };

  [[nodiscard]] auto HandleRequest(const QJsonObject& request) -> QJsonObject;
  [[nodiscard]] auto Snapshot() const -> QJsonObject;
  [[nodiscard]] auto FindTarget(const QString& target) const -> std::optional<TargetMatch>;
  [[nodiscard]] auto SummarizeItem(QQuickItem* item, const QStringList& path) const -> QJsonObject;

  void               SendJson(const QJsonObject& object);
  void               SendReadyEvent();
  void SendError(const QJsonValue& request_id, const QString& code, const QString& message);

  [[nodiscard]] auto              BaseResponse(const QJsonObject& request) const -> QJsonObject;
  [[nodiscard]] static auto       VariantToJson(const QVariant& value) -> QJsonValue;
  [[nodiscard]] static auto       TargetName(QQuickItem* item) -> QString;
  [[nodiscard]] static auto       TestId(QQuickItem* item) -> QString;

  QPointer<QQmlApplicationEngine> engine_;
  QPointer<QQuickWindow>          window_;
  QLocalServer                    server_;
  QPointer<QLocalSocket>          client_;
  QTimer                          heartbeat_timer_;
  QElapsedTimer                   elapsed_timer_;
  QString                         socket_name_;
  quint64                         heartbeat_counter_ = 0;
  bool                            ready_             = false;
};

}  // namespace alcedo::ui
