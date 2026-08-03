//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "test_probe.hpp"

#include <QColor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaProperty>
#include <QQmlApplicationEngine>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QThread>
#include <QUrl>
#include <functional>

namespace alcedo::ui {
namespace {

auto JsonPath(const QStringList& path) -> QJsonArray {
  QJsonArray result;
  for (const QString& part : path) {
    result.append(part);
  }
  return result;
}

auto JsonObjectForRequest(const QJsonObject& request) -> QJsonObject {
  QJsonObject response;
  if (request.contains(QStringLiteral("id"))) {
    response.insert(QStringLiteral("id"), request.value(QStringLiteral("id")));
  }
  return response;
}

auto SceneRectForItem(QQuickItem* item) -> QRectF {
  if (item == nullptr) {
    return {};
  }
  if (item->window() != nullptr) {
    return item->mapRectToScene(item->boundingRect());
  }
  return item->boundingRect();
}

auto ErrorResponse(const QJsonObject& request, const QString& code, const QString& message)
    -> QJsonObject {
  QJsonObject response = JsonObjectForRequest(request);
  response.insert(QStringLiteral("ok"), false);
  QJsonObject error;
  error.insert(QStringLiteral("code"), code);
  error.insert(QStringLiteral("message"), message);
  response.insert(QStringLiteral("error"), error);
  return response;
}

}  // namespace

TestProbe::TestProbe(QQmlApplicationEngine* engine, QQuickWindow* window, QObject* parent)
    : QObject(parent), engine_(engine), window_(window), server_(this), heartbeat_timer_(this) {
  heartbeat_timer_.setInterval(250);
  heartbeat_timer_.setTimerType(Qt::PreciseTimer);
  connect(&server_, &QLocalServer::newConnection, this, &TestProbe::HandleNewConnection);
  connect(&heartbeat_timer_, &QTimer::timeout, this, &TestProbe::EmitHeartbeat);
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
    SendJson(HandleRequest(document.object()));
  }
}

void TestProbe::HandleSocketDisconnected() {
  auto* socket = qobject_cast<QLocalSocket*>(sender());
  if (socket == nullptr || socket != client_) {
    return;
  }
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

void TestProbe::SendError(const QJsonValue& requestId, const QString& code,
                          const QString& message) {
  QJsonObject response;
  if (!requestId.isUndefined()) {
    response.insert(QStringLiteral("id"), requestId);
  }
  response.insert(QStringLiteral("ok"), false);
  QJsonObject error;
  error.insert(QStringLiteral("code"), code);
  error.insert(QStringLiteral("message"), message);
  response.insert(QStringLiteral("error"), error);
  SendJson(response);
}

auto TestProbe::BaseResponse(const QJsonObject& request) const -> QJsonObject {
  return JsonObjectForRequest(request);
}

auto TestProbe::HandleRequest(const QJsonObject& request) -> QJsonObject {
  const QString method = request.value(QStringLiteral("method")).toString().trimmed();
  if (method == QStringLiteral("ping")) {
    QJsonObject response = BaseResponse(request);
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
    QJsonObject response = BaseResponse(request);
    response.insert(QStringLiteral("ok"), true);
    response.insert(QStringLiteral("result"), Snapshot());
    return response;
  }

  if (method == QStringLiteral("find")) {
    const QString target = request.value(QStringLiteral("target")).toString().trimmed();
    const auto    match  = FindTarget(target);
    if (!match.has_value()) {
      return ErrorResponse(request, QStringLiteral("target_not_found"),
                           QStringLiteral("No live QML item matched '%1'.").arg(target));
    }

    QJsonObject response = BaseResponse(request);
    response.insert(QStringLiteral("ok"), true);
    QJsonObject result;
    result.insert(QStringLiteral("found"), true);
    result.insert(QStringLiteral("element"), SummarizeItem(match->item, match->path));
    result.insert(QStringLiteral("path"), JsonPath(match->path));
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
      return ErrorResponse(request, QStringLiteral("invalid_read_target"),
                           QStringLiteral("read requires an item and property."));
    }

    const auto match = FindTarget(target);
    if (!match.has_value()) {
      return ErrorResponse(request, QStringLiteral("target_not_found"),
                           QStringLiteral("No live QML item matched '%1'.").arg(target));
    }

    QVariant value;
    if (property == QStringLiteral("visible")) {
      value = match->item->isVisible();
    } else if (property == QStringLiteral("enabled")) {
      value = match->item->isEnabled();
    } else if (property == QStringLiteral("activeFocus")) {
      value = match->item->hasActiveFocus();
    } else {
      const QQmlProperty qml_property(match->item, property);
      if (!qml_property.isValid()) {
        return ErrorResponse(
            request, QStringLiteral("property_not_found"),
            QStringLiteral("Item '%1' has no readable property '%2'.").arg(target, property));
      }
      value = qml_property.read();
    }

    QJsonObject response = BaseResponse(request);
    response.insert(QStringLiteral("ok"), true);
    QJsonObject result;
    result.insert(QStringLiteral("target"), target);
    result.insert(QStringLiteral("property"), property);
    result.insert(QStringLiteral("value"), VariantToJson(value));
    response.insert(QStringLiteral("result"), result);
    return response;
  }

  return ErrorResponse(request, QStringLiteral("unsupported_method"),
                       QStringLiteral("Supported methods are snapshot, find, read, and ping."));
}

auto TestProbe::Snapshot() const -> QJsonObject {
  QJsonObject result;
  result.insert(QStringLiteral("engineLoaded"),
                engine_ != nullptr && !engine_->rootObjects().isEmpty());
  result.insert(QStringLiteral("windowTitle"), window_ != nullptr ? window_->title() : QString());
  result.insert(QStringLiteral("windowVisible"), window_ != nullptr && window_->isVisible());

  QJsonArray elements;
  if (window_ != nullptr && window_->contentItem() != nullptr) {
    const std::function<void(QQuickItem*, const QStringList&)> append_items =
        [&append_items, &elements, this](QQuickItem* item, const QStringList& parent_path) {
          if (item == nullptr) {
            return;
          }
          QStringList   path = parent_path;
          const QString name = TargetName(item);
          if (!name.isEmpty()) {
            path.append(name);
          }
          elements.append(SummarizeItem(item, path));
          for (QQuickItem* child : item->childItems()) {
            append_items(child, path);
          }
        };
    append_items(window_->contentItem(), {});
  }
  result.insert(QStringLiteral("elements"), elements);
  return result;
}

auto TestProbe::FindTarget(const QString& target) const -> std::optional<TargetMatch> {
  if (target.isEmpty() || window_ == nullptr || window_->contentItem() == nullptr) {
    return std::nullopt;
  }

  const std::function<std::optional<TargetMatch>(QQuickItem*, const QStringList&)> find_item =
      [&find_item, &target](QQuickItem*        item,
                            const QStringList& parent_path) -> std::optional<TargetMatch> {
    if (item == nullptr) {
      return std::nullopt;
    }
    QStringList   path = parent_path;
    const QString name = TestProbe::TargetName(item);
    if (!name.isEmpty()) {
      path.append(name);
    }
    if (item->objectName() == target || TestProbe::TestId(item) == target ||
        path.join('.') == target) {
      return TargetMatch{item, path};
    }
    for (QQuickItem* child : item->childItems()) {
      if (const auto found = find_item(child, path); found.has_value()) {
        return found;
      }
    }
    return std::nullopt;
  };

  return find_item(window_->contentItem(), {});
}

auto TestProbe::SummarizeItem(QQuickItem* item, const QStringList& path) const -> QJsonObject {
  QJsonObject result;
  if (item == nullptr) {
    return result;
  }

  const QString target_name = TargetName(item);
  if (!target_name.isEmpty()) {
    result.insert(QStringLiteral("id"), target_name);
  }
  const QString test_id = TestId(item);
  if (!test_id.isEmpty()) {
    result.insert(QStringLiteral("testId"), test_id);
  }
  result.insert(QStringLiteral("objectName"), item->objectName());
  result.insert(QStringLiteral("type"), QString::fromLatin1(item->metaObject()->className()));
  result.insert(QStringLiteral("visible"), item->isVisible());
  result.insert(QStringLiteral("enabled"), item->isEnabled());
  result.insert(QStringLiteral("activeFocus"), item->hasActiveFocus());
  result.insert(QStringLiteral("path"), JsonPath(path));

  const QRectF scene_rect = SceneRectForItem(item);
  QJsonObject  rect;
  rect.insert(QStringLiteral("x"), scene_rect.x());
  rect.insert(QStringLiteral("y"), scene_rect.y());
  rect.insert(QStringLiteral("width"), scene_rect.width());
  rect.insert(QStringLiteral("height"), scene_rect.height());
  result.insert(QStringLiteral("sceneRect"), rect);

  for (const char* property_name : {"text", "checked", "currentIndex", "value"}) {
    const QVariant value = item->property(property_name);
    if (value.isValid()) {
      result.insert(QString::fromLatin1(property_name), VariantToJson(value));
    }
  }
  return result;
}

auto TestProbe::VariantToJson(const QVariant& value) -> QJsonValue {
  if (!value.isValid() || value.isNull()) {
    return QJsonValue(QJsonValue::Null);
  }

  switch (value.typeId()) {
    case QMetaType::Bool:
      return QJsonValue(value.toBool());
    case QMetaType::Int:
    case QMetaType::Short:
    case QMetaType::Char16:
    case QMetaType::Char32:
      return QJsonValue(value.toInt());
    case QMetaType::UInt:
    case QMetaType::UShort:
    case QMetaType::UChar:
      return QJsonValue(static_cast<qint64>(value.toUInt()));
    case QMetaType::LongLong:
      return QJsonValue(value.toLongLong());
    case QMetaType::ULongLong:
      return QJsonValue(static_cast<qint64>(value.toULongLong()));
    case QMetaType::Float:
    case QMetaType::Double:
      return QJsonValue(value.toDouble());
    case QMetaType::QString:
      return QJsonValue(value.toString());
    case QMetaType::QByteArray:
      return QJsonValue(QString::fromUtf8(value.toByteArray()));
    case QMetaType::QUrl:
      return QJsonValue(value.toUrl().toString());
    case QMetaType::QColor:
      return QJsonValue(value.value<QColor>().name(QColor::HexArgb));
    case QMetaType::QStringList: {
      QJsonArray result;
      for (const QString& entry : value.toStringList()) {
        result.append(entry);
      }
      return result;
    }
    case QMetaType::QVariantList: {
      QJsonArray result;
      for (const QVariant& entry : value.toList()) {
        result.append(VariantToJson(entry));
      }
      return result;
    }
    case QMetaType::QVariantMap: {
      QJsonObject       result;
      const QVariantMap map = value.toMap();
      for (auto it = map.cbegin(); it != map.cend(); ++it) {
        result.insert(it.key(), VariantToJson(it.value()));
      }
      return result;
    }
    default:
      return QJsonValue(value.toString());
  }
}

auto TestProbe::TargetName(QQuickItem* item) -> QString {
  if (item == nullptr) {
    return {};
  }
  if (!item->objectName().isEmpty()) {
    return item->objectName();
  }
  return TestId(item);
}

auto TestProbe::TestId(QQuickItem* item) -> QString {
  if (item == nullptr) {
    return {};
  }
  const QVariant value = item->property("testId");
  return value.isValid() ? value.toString() : QString();
}

}  // namespace alcedo::ui
