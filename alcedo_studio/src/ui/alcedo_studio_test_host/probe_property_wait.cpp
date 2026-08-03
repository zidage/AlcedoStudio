//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "probe_property_wait.hpp"

#include "probe_item_tree.hpp"
#include "probe_json.hpp"

#include <QMetaMethod>
#include <QMetaProperty>
#include <QQuickItem>
#include <algorithm>
#include <utility>

namespace alcedo::ui {
namespace {

constexpr int kDefaultWaitTimeoutMs = 8000;
constexpr int kWaitPollIntervalMs   = 16;

}  // namespace

ProbePropertyWait::ProbePropertyWait(ProbeItemTree* tree, QObject* parent)
    : QObject(parent), tree_(tree) {}

ProbePropertyWait::~ProbePropertyWait() {
  Cancel();
}

void ProbePropertyWait::Begin(const QJsonObject& request) {
  if (pending_.has_value()) {
    QJsonObject response;
    if (request.contains(QStringLiteral("id"))) {
      response.insert(QStringLiteral("id"), request.value(QStringLiteral("id")));
    }
    response.insert(QStringLiteral("ok"), false);
    QJsonObject error;
    error.insert(QStringLiteral("code"), QStringLiteral("wait_busy"));
    error.insert(QStringLiteral("message"),
                 QStringLiteral("Another wait is already in progress."));
    response.insert(QStringLiteral("error"), error);
    emit ReplyReady(response);
    return;
  }

  if (tree_ == nullptr) {
    QJsonObject response = probe_json::ErrorResponse(
        request, QStringLiteral("no_tree"), QStringLiteral("Probe item tree is unavailable."));
    emit ReplyReady(response);
    return;
  }

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
    emit ReplyReady(probe_json::ErrorResponse(
        request, QStringLiteral("invalid_wait_target"),
        QStringLiteral("wait requires an item and property.")));
    return;
  }

  const auto [matcher, expected] = ParseMatcher(request);
  if (matcher.isEmpty()) {
    emit ReplyReady(probe_json::ErrorResponse(
        request, QStringLiteral("invalid_matcher"),
        QStringLiteral("wait requires one matcher such as eq, gte, or truthy.")));
    return;
  }

  const int timeout_ms = std::max(
      1, request.value(QStringLiteral("timeoutMs"))
             .toInt(request.value(QStringLiteral("timeout")).toInt(kDefaultWaitTimeoutMs)));

  PendingWait wait;
  wait.request_id    = request.value(QStringLiteral("id"));
  wait.target        = target;
  wait.property_name = property;
  wait.matcher       = matcher;
  wait.expected      = expected;

  auto* poll = new QTimer(this);
  poll->setInterval(kWaitPollIntervalMs);
  poll->setTimerType(Qt::PreciseTimer);
  connect(poll, &QTimer::timeout, this, &ProbePropertyWait::Evaluate);
  wait.poll_timer = poll;

  auto* timeout = new QTimer(this);
  timeout->setSingleShot(true);
  timeout->setInterval(timeout_ms);
  connect(timeout, &QTimer::timeout, this, &ProbePropertyWait::FinishTimeout);
  wait.timeout_timer = timeout;

  pending_ = std::move(wait);

  Evaluate();
  if (!pending_.has_value()) {
    return;
  }

  SubscribeNotifyIfAvailable();
  pending_->poll_timer->start();
  pending_->timeout_timer->start();
}

void ProbePropertyWait::Cancel() {
  ClearPending(false, false);
}

void ProbePropertyWait::Evaluate() {
  if (!pending_.has_value() || tree_ == nullptr) {
    return;
  }

  const auto match = tree_->FindTarget(pending_->target);
  if (!match.has_value()) {
    pending_->last_actual = QVariant();
    return;
  }

  const auto value = tree_->ReadItemProperty(match->item, pending_->property_name);
  if (!value.has_value()) {
    pending_->last_actual = QVariant();
    return;
  }
  pending_->last_actual = *value;
  if (MatcherHolds(pending_->matcher, *value, pending_->expected)) {
    ClearPending(true, true);
  }
}

void ProbePropertyWait::FinishTimeout() {
  ClearPending(true, false);
}

void ProbePropertyWait::SubscribeNotifyIfAvailable() {
  if (!pending_.has_value() || tree_ == nullptr) {
    return;
  }
  const auto match = tree_->FindTarget(pending_->target);
  if (!match.has_value()) {
    return;
  }
  const QMetaObject* meta = match->item->metaObject();
  const int idx = meta->indexOfProperty(pending_->property_name.toUtf8().constData());
  if (idx < 0) {
    return;
  }
  const QMetaProperty prop = meta->property(idx);
  if (!prop.hasNotifySignal()) {
    return;
  }
  const QMetaMethod signal     = prop.notifySignal();
  const int         slot_index = metaObject()->indexOfSlot("Evaluate()");
  if (slot_index < 0) {
    return;
  }
  const QMetaMethod slot         = metaObject()->method(slot_index);
  pending_->notify_connection = QObject::connect(match->item, signal, this, slot);
}

void ProbePropertyWait::ClearPending(bool emit_reply, bool matched) {
  if (!pending_.has_value()) {
    return;
  }

  PendingWait finished = std::move(*pending_);
  pending_.reset();

  if (finished.notify_connection) {
    QObject::disconnect(finished.notify_connection);
  }
  if (finished.poll_timer != nullptr) {
    finished.poll_timer->stop();
    finished.poll_timer->deleteLater();
  }
  if (finished.timeout_timer != nullptr) {
    finished.timeout_timer->stop();
    finished.timeout_timer->deleteLater();
  }

  if (!emit_reply) {
    return;
  }

  if (matched) {
    QJsonObject response;
    if (!finished.request_id.isUndefined()) {
      response.insert(QStringLiteral("id"), finished.request_id);
    }
    response.insert(QStringLiteral("ok"), true);
    response.insert(QStringLiteral("result"), QStringLiteral("ok"));
    response.insert(QStringLiteral("actual"), probe_json::VariantToJson(finished.last_actual));
    emit ReplyReady(response);
    return;
  }

  QJsonObject response;
  if (!finished.request_id.isUndefined()) {
    response.insert(QStringLiteral("id"), finished.request_id);
  }
  response.insert(QStringLiteral("ok"), false);
  QJsonObject error;
  error.insert(QStringLiteral("code"), QStringLiteral("wait_timeout"));
  error.insert(QStringLiteral("message"),
               QStringLiteral("Timed out waiting for '%1.%2'.")
                   .arg(finished.target, finished.property_name));
  error.insert(QStringLiteral("actual"), probe_json::VariantToJson(finished.last_actual));
  error.insert(QStringLiteral("target"), finished.target);
  error.insert(QStringLiteral("property"), finished.property_name);
  response.insert(QStringLiteral("error"), error);
  emit ReplyReady(response);
}

bool ProbePropertyWait::MatcherHolds(const QString& matcher, const QVariant& actual,
                                     const QJsonValue& expected) {
  if (matcher == QStringLiteral("truthy")) {
    if (actual.typeId() == QMetaType::Bool) {
      return actual.toBool();
    }
    if (actual.canConvert<double>()) {
      return actual.toDouble() != 0.0;
    }
    return !actual.toString().isEmpty();
  }
  if (matcher == QStringLiteral("eq")) {
    if (expected.isBool()) {
      return actual.toBool() == expected.toBool();
    }
    if (expected.isDouble()) {
      return qFuzzyCompare(actual.toDouble() + 1.0, expected.toDouble() + 1.0) ||
             actual.toDouble() == expected.toDouble();
    }
    if (expected.isString()) {
      return actual.toString() == expected.toString();
    }
    if (expected.isNull()) {
      return !actual.isValid() || actual.isNull();
    }
    return probe_json::VariantToJson(actual) == expected;
  }
  if (matcher == QStringLiteral("ne")) {
    return !MatcherHolds(QStringLiteral("eq"), actual, expected);
  }
  if (matcher == QStringLiteral("contains")) {
    return actual.toString().contains(expected.toString());
  }
  if (matcher == QStringLiteral("gt")) {
    return actual.toDouble() > expected.toDouble();
  }
  if (matcher == QStringLiteral("gte")) {
    return actual.toDouble() >= expected.toDouble();
  }
  if (matcher == QStringLiteral("lt")) {
    return actual.toDouble() < expected.toDouble();
  }
  if (matcher == QStringLiteral("lte")) {
    return actual.toDouble() <= expected.toDouble();
  }
  return false;
}

auto ProbePropertyWait::ParseMatcher(const QJsonObject& request)
    -> std::pair<QString, QJsonValue> {
  static const char* kMatchers[] = {"eq", "ne", "contains", "gt", "gte", "lt", "lte", "truthy"};
  for (const char* name : kMatchers) {
    const QString key = QString::fromLatin1(name);
    if (!request.contains(key)) {
      continue;
    }
    if (key == QStringLiteral("truthy")) {
      return {key, QJsonValue(true)};
    }
    return {key, request.value(key)};
  }
  if (request.contains(QStringLiteral("matcher"))) {
    const QString matcher = request.value(QStringLiteral("matcher")).toString().trimmed();
    return {matcher, request.value(QStringLiteral("value"))};
  }
  return {};
}

}  // namespace alcedo::ui
