//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "test_probe.hpp"

#include "probe_json.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QThread>

namespace alcedo::ui {

TestProbe::TestProbe(QQmlApplicationEngine* engine, QQuickWindow* window, QObject* parent)
    : QObject(parent),
      window_(window),
      tree_(engine, window),
      input_(&tree_, window),
      wait_(&tree_, this),
      server_(this),
      heartbeat_timer_(this) {
  heartbeat_timer_.setInterval(250);
  heartbeat_timer_.setTimerType(Qt::PreciseTimer);
  connect(&server_, &QLocalServer::newConnection, this, &TestProbe::HandleNewConnection);
  connect(&heartbeat_timer_, &QTimer::timeout, this, &TestProbe::EmitHeartbeat);
  connect(&wait_, &ProbePropertyWait::ReplyReady, this, &TestProbe::ForwardWaitReply);
}

bool TestProbe::Start(const QString& socketName, QString* error) {
  if (server_.isListening()) {
    if (error != nullptr) {
      *error = QStringLiteral("Probe server is already listening.");
    }
    return false;
  }

  socket_name_ = socketName.trimmed();
  if (socket_name_.isEmpty()) {
    if (error != nullptr) {
      *error = QStringLiteral("Probe socket name cannot be empty.");
    }
    return false;
  }

  if (!server_.listen(socket_name_)) {
    if (error != nullptr) {
      *error = server_.errorString();
    }
    return false;
  }

  elapsed_timer_.start();
  heartbeat_timer_.start();
  return true;
}

void TestProbe::MarkReady() {
  if (ready_) {
    return;
  }
  ready_ = true;
  SendReadyEvent();
}

void TestProbe::HandleNewConnection() {
  while (server_.hasPendingConnections()) {
    QLocalSocket* incoming = server_.nextPendingConnection();
    if (incoming == nullptr) {
      continue;
    }

    if (client_ != nullptr && client_->state() != QLocalSocket::UnconnectedState) {
      incoming->disconnectFromServer();
      incoming->deleteLater();
      continue;
    }

    client_ = incoming;
    incoming->setParent(this);
    connect(incoming, &QLocalSocket::readyRead, this, &TestProbe::HandleReadyRead);
    connect(incoming, &QLocalSocket::disconnected, this, &TestProbe::HandleSocketDisconnected);
    if (ready_) {
      SendReadyEvent();
    }
  }
}

void TestProbe::HandleReadyRead() {
  auto* socket = qobject_cast<QLocalSocket*>(sender());
  if (socket == nullptr || socket != client_) {
    return;
  }

  while (socket->canReadLine()) {
    const QByteArray line = socket->readLine().trimmed();
    if (line.isEmpty()) {
      continue;
    }

    QJsonParseError     parse_error{};
    const QJsonDocument document = QJsonDocument::fromJson(line, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
      SendError(QJsonValue(), QStringLiteral("invalid_json"),
                QStringLiteral("Each probe request must be one JSON object per line."));
      continue;
    }

    const QJsonObject request = document.object();
    const QString     method  = request.value(QStringLiteral("method")).toString().trimmed();
    if (method == QStringLiteral("wait")) {
      wait_.Begin(request);
      continue;
    }
    SendJson(HandleRequest(request));
  }
}

void TestProbe::HandleSocketDisconnected() {
  auto* socket = qobject_cast<QLocalSocket*>(sender());
  if (socket == nullptr || socket != client_) {
    return;
  }
  wait_.Cancel();
  client_.clear();
  socket->deleteLater();
}

void TestProbe::EmitHeartbeat() {
  ++heartbeat_counter_;
  QJsonObject event;
  event.insert(QStringLiteral("event"), QStringLiteral("heartbeat"));
  event.insert(QStringLiteral("counter"), static_cast<qint64>(heartbeat_counter_));
  event.insert(QStringLiteral("guiTimeMs"),
               elapsed_timer_.isValid() ? elapsed_timer_.elapsed() : static_cast<qint64>(0));
  event.insert(QStringLiteral("guiThread"), QThread::currentThread() == thread());
  event.insert(QStringLiteral("ready"), ready_);
  SendJson(event);
}

void TestProbe::ForwardWaitReply(const QJsonObject& response) {
  SendJson(response);
}

void TestProbe::SendJson(const QJsonObject& object) {
  if (client_ == nullptr || client_->state() != QLocalSocket::ConnectedState) {
    return;
  }

  QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact);
  line.append('\n');
  client_->write(line);
  client_->flush();
}

void TestProbe::SendReadyEvent() {
  QJsonObject event;
  event.insert(QStringLiteral("event"), QStringLiteral("ready"));
  event.insert(QStringLiteral("windowVisible"), window_ != nullptr && window_->isVisible());
  event.insert(QStringLiteral("windowTitle"), window_ != nullptr ? window_->title() : QString());
  SendJson(event);
}

void TestProbe::SendError(const QJsonValue& requestId, const QString& code, const QString& message,
                          const QJsonObject& extra) {
  QJsonObject response;
  if (!requestId.isUndefined()) {
    response.insert(QStringLiteral("id"), requestId);
  }
  response.insert(QStringLiteral("ok"), false);
  QJsonObject error;
  error.insert(QStringLiteral("code"), code);
  error.insert(QStringLiteral("message"), message);
  for (auto it = extra.constBegin(); it != extra.constEnd(); ++it) {
    error.insert(it.key(), it.value());
  }
  response.insert(QStringLiteral("error"), error);
  SendJson(response);
}

auto TestProbe::HandleRequest(const QJsonObject& request) -> QJsonObject {
  const QString method = request.value(QStringLiteral("method")).toString().trimmed();
  if (method == QStringLiteral("ping")) {
    QJsonObject response = probe_json::BaseResponse(request);
    response.insert(QStringLiteral("ok"), true);
    QJsonObject result;
    result.insert(QStringLiteral("status"), QStringLiteral("ok"));
    result.insert(QStringLiteral("heartbeat"), static_cast<qint64>(heartbeat_counter_));
    result.insert(QStringLiteral("guiThread"), QThread::currentThread() == thread());
    result.insert(QStringLiteral("ready"), ready_);
    response.insert(QStringLiteral("result"), result);
    return response;
  }

  if (method == QStringLiteral("snapshot")) {
    QJsonObject response = probe_json::BaseResponse(request);
    response.insert(QStringLiteral("ok"), true);
    response.insert(QStringLiteral("result"), tree_.Snapshot());
    return response;
  }

  if (method == QStringLiteral("find")) {
    const QString target = request.value(QStringLiteral("target")).toString().trimmed();
    const auto    match  = tree_.FindTarget(target);
    if (!match.has_value()) {
      return probe_json::ErrorResponse(
          request, QStringLiteral("target_not_found"),
          QStringLiteral("No live QML item matched '%1'.").arg(target));
    }

    QJsonObject response = probe_json::BaseResponse(request);
    response.insert(QStringLiteral("ok"), true);
    QJsonObject result;
    result.insert(QStringLiteral("found"), true);
    result.insert(QStringLiteral("element"), tree_.SummarizeItem(match->item, match->path));
    result.insert(QStringLiteral("path"), probe_json::JsonPath(match->path));
    response.insert(QStringLiteral("result"), result);
    return response;
  }

  if (method == QStringLiteral("read")) {
    QString target   = request.value(QStringLiteral("target")).toString().trimmed();
    QString property = request.value(QStringLiteral("property")).toString().trimmed();
    if (property.isEmpty()) {
      const int separator = target.lastIndexOf('.');
      if (separator > 0) {
        property = target.mid(separator + 1);
        target   = target.left(separator);
      }
    }
    if (target.isEmpty() || property.isEmpty()) {
      return probe_json::ErrorResponse(request, QStringLiteral("invalid_read_target"),
                                       QStringLiteral("read requires an item and property."));
    }

    const auto match = tree_.FindTarget(target);
    if (!match.has_value()) {
      return probe_json::ErrorResponse(
          request, QStringLiteral("target_not_found"),
          QStringLiteral("No live QML item matched '%1'.").arg(target));
    }

    const auto value = tree_.ReadItemProperty(match->item, property);
    if (!value.has_value()) {
      return probe_json::ErrorResponse(
          request, QStringLiteral("property_not_found"),
          QStringLiteral("Item '%1' has no readable property '%2'.").arg(target, property));
    }

    QJsonObject response = probe_json::BaseResponse(request);
    response.insert(QStringLiteral("ok"), true);
    QJsonObject result;
    result.insert(QStringLiteral("target"), target);
    result.insert(QStringLiteral("property"), property);
    result.insert(QStringLiteral("value"), probe_json::VariantToJson(*value));
    response.insert(QStringLiteral("result"), result);
    return response;
  }

  if (method == QStringLiteral("click")) {
    return input_.HandleClick(request, ProbeInputInjector::ClickKind::Single);
  }
  if (method == QStringLiteral("doubleClick")) {
    return input_.HandleClick(request, ProbeInputInjector::ClickKind::Double);
  }
  if (method == QStringLiteral("rightClick")) {
    return input_.HandleClick(request, ProbeInputInjector::ClickKind::Right);
  }
  if (method == QStringLiteral("key")) {
    return input_.HandleKey(request);
  }
  if (method == QStringLiteral("typeText")) {
    return input_.HandleTypeText(request);
  }
  if (method == QStringLiteral("drag")) {
    return input_.HandleDrag(request);
  }
  if (method == QStringLiteral("screenshot")) {
    return input_.HandleScreenshot(request);
  }

  return probe_json::ErrorResponse(
      request, QStringLiteral("unsupported_method"),
      QStringLiteral(
          "Supported methods are snapshot, find, read, click, doubleClick, rightClick, key, "
          "typeText, drag, wait, screenshot, and ping."));
}

}  // namespace alcedo::ui
