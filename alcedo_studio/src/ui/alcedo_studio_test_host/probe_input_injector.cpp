//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "probe_input_injector.hpp"

#include "probe_item_tree.hpp"
#include "probe_json.hpp"

#include <QBuffer>
#include <QCoreApplication>
#include <QEventLoop>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QQuickItem>
#include <QQuickWindow>
#include <algorithm>

namespace alcedo::ui {
namespace {

constexpr int kDefaultDragSteps = 8;

}  // namespace

ProbeInputInjector::ProbeInputInjector(ProbeItemTree* tree, QQuickWindow* window)
    : tree_(tree), window_(window) {}

auto ProbeInputInjector::HandleClick(const QJsonObject& request, ClickKind kind) -> QJsonObject {
  if (window_ == nullptr || tree_ == nullptr) {
    return probe_json::ErrorResponse(request, QStringLiteral("no_window"),
                                     QStringLiteral("Probe has no QQuickWindow."));
  }
  const QString target = request.value(QStringLiteral("target")).toString().trimmed();
  const auto    match  = tree_->FindTarget(target);
  if (!match.has_value()) {
    return probe_json::ErrorResponse(
        request, QStringLiteral("target_not_found"),
        QStringLiteral("No live QML item matched '%1'.").arg(target));
  }

  if (const auto rejection = ValidateClickable(match->item); rejection.has_value()) {
    return probe_json::ErrorResponse(
        request, *rejection,
        QStringLiteral("Target '%1' rejected for %2.").arg(target, *rejection));
  }

  const QPointF scene_pos = SceneCenter(match->item);
  const Qt::MouseButton button =
      (kind == ClickKind::Right) ? Qt::RightButton : Qt::LeftButton;
  DeliverMouseClick(scene_pos, button, kind == ClickKind::Double);
  return probe_json::OkStatusResponse(request);
}

auto ProbeInputInjector::HandleKey(const QJsonObject& request) -> QJsonObject {
  if (window_ == nullptr) {
    return probe_json::ErrorResponse(request, QStringLiteral("no_window"),
                                     QStringLiteral("Probe has no QQuickWindow."));
  }
  const int key = request.value(QStringLiteral("key")).toInt(-1);
  if (key < 0) {
    return probe_json::ErrorResponse(request, QStringLiteral("invalid_key"),
                                     QStringLiteral("key requires an integer Qt::Key value."));
  }
  const QString text = request.value(QStringLiteral("text")).toString();
  Qt::KeyboardModifiers modifiers = Qt::NoModifier;
  if (request.value(QStringLiteral("ctrl")).toBool()) {
    modifiers |= Qt::ControlModifier;
  }
  if (request.value(QStringLiteral("shift")).toBool()) {
    modifiers |= Qt::ShiftModifier;
  }
  if (request.value(QStringLiteral("alt")).toBool()) {
    modifiers |= Qt::AltModifier;
  }
  DeliverKey(key, modifiers, text);
  return probe_json::OkStatusResponse(request);
}

auto ProbeInputInjector::HandleTypeText(const QJsonObject& request) -> QJsonObject {
  if (window_ == nullptr) {
    return probe_json::ErrorResponse(request, QStringLiteral("no_window"),
                                     QStringLiteral("Probe has no QQuickWindow."));
  }
  const QString text = request.value(QStringLiteral("text")).toString();
  if (text.isEmpty()) {
    return probe_json::ErrorResponse(request, QStringLiteral("invalid_text"),
                                     QStringLiteral("typeText requires a non-empty text string."));
  }
  for (const QChar ch : text) {
    DeliverKey(ch.unicode(), Qt::NoModifier, QString(ch));
  }
  return probe_json::OkStatusResponse(request);
}

auto ProbeInputInjector::HandleDrag(const QJsonObject& request) -> QJsonObject {
  if (window_ == nullptr || tree_ == nullptr) {
    return probe_json::ErrorResponse(request, QStringLiteral("no_window"),
                                     QStringLiteral("Probe has no QQuickWindow."));
  }
  const QString target = request.value(QStringLiteral("target")).toString().trimmed();
  const auto    match  = tree_->FindTarget(target);
  if (!match.has_value()) {
    return probe_json::ErrorResponse(
        request, QStringLiteral("target_not_found"),
        QStringLiteral("No live QML item matched '%1'.").arg(target));
  }
  if (const auto rejection = ValidateClickable(match->item); rejection.has_value()) {
    return probe_json::ErrorResponse(
        request, *rejection,
        QStringLiteral("Target '%1' rejected for %2.").arg(target, *rejection));
  }

  const QRectF bounds = match->item->boundingRect();
  const qreal  from_nx =
      request.contains(QStringLiteral("fromNx")) ? request.value(QStringLiteral("fromNx")).toDouble()
                                                 : 0.1;
  const qreal to_nx = request.contains(QStringLiteral("toNx"))
                          ? request.value(QStringLiteral("toNx")).toDouble()
                          : 0.9;
  const qreal ny =
      request.contains(QStringLiteral("ny")) ? request.value(QStringLiteral("ny")).toDouble() : 0.5;
  const int steps = std::max(1, request.value(QStringLiteral("steps")).toInt(kDefaultDragSteps));

  const QPointF from = match->item->mapToScene(
      QPointF(bounds.x() + bounds.width() * from_nx, bounds.y() + bounds.height() * ny));
  const QPointF to = match->item->mapToScene(
      QPointF(bounds.x() + bounds.width() * to_nx, bounds.y() + bounds.height() * ny));
  DeliverMouseDrag(from, to, steps);
  return probe_json::OkStatusResponse(request);
}

auto ProbeInputInjector::HandleScreenshot(const QJsonObject& request) -> QJsonObject {
  if (window_ == nullptr) {
    return probe_json::ErrorResponse(request, QStringLiteral("no_window"),
                                     QStringLiteral("Probe has no QQuickWindow."));
  }
  const QImage image = window_->grabWindow();
  if (image.isNull()) {
    return probe_json::ErrorResponse(
        request, QStringLiteral("screenshot_failed"),
        QStringLiteral("QQuickWindow::grabWindow returned a null image."));
  }
  QByteArray png;
  QBuffer    buffer(&png);
  buffer.open(QIODevice::WriteOnly);
  if (!image.save(&buffer, "PNG")) {
    return probe_json::ErrorResponse(
        request, QStringLiteral("screenshot_failed"),
        QStringLiteral("Failed to encode grabWindow output as PNG."));
  }

  QJsonObject response = probe_json::BaseResponse(request);
  response.insert(QStringLiteral("ok"), true);
  QJsonObject result;
  result.insert(QStringLiteral("format"), QStringLiteral("png"));
  result.insert(QStringLiteral("byteLength"), png.size());
  result.insert(QStringLiteral("width"), image.width());
  result.insert(QStringLiteral("height"), image.height());
  result.insert(QStringLiteral("pngBase64"), QString::fromLatin1(png.toBase64()));
  response.insert(QStringLiteral("result"), result);
  return response;
}

auto ProbeInputInjector::ValidateClickable(QQuickItem* item) const -> std::optional<QString> {
  if (item == nullptr) {
    return QStringLiteral("target_not_found");
  }
  if (!IsEffectivelyVisible(item) || item->width() <= 0.0 || item->height() <= 0.0) {
    return QStringLiteral("target_invisible");
  }
  if (!item->isEnabled()) {
    return QStringLiteral("target_disabled");
  }
  if (window_ == nullptr || window_->contentItem() == nullptr) {
    return QStringLiteral("no_window");
  }
  const QPointF center  = SceneCenter(item);
  QQuickItem*   topmost = TopmostItemAt(window_->contentItem(), center);
  if (IsCoveredByModalOverlay(item, topmost)) {
    return QStringLiteral("target_covered");
  }
  return std::nullopt;
}

auto ProbeInputInjector::SceneCenter(QQuickItem* item) const -> QPointF {
  return item->mapToScene(item->boundingRect().center());
}

void ProbeInputInjector::DeliverMouseClick(const QPointF& scene_pos, Qt::MouseButton button,
                                           bool double_click) {
  if (window_ == nullptr) {
    return;
  }
  const QPointF local  = scene_pos;
  const QPointF global = window_->mapToGlobal(scene_pos.toPoint());

  auto send_mouse = [this, local, global, scene_pos](QEvent::Type type, Qt::MouseButton btn,
                                                     Qt::MouseButtons buttons) {
    QMouseEvent event(type, local, scene_pos, global, btn, buttons, Qt::NoModifier);
    QCoreApplication::sendEvent(window_, &event);
  };

  send_mouse(QEvent::MouseMove, Qt::NoButton, Qt::NoButton);

  if (double_click) {
    send_mouse(QEvent::MouseButtonPress, button, button);
    send_mouse(QEvent::MouseButtonRelease, button, Qt::NoButton);
    send_mouse(QEvent::MouseButtonPress, button, button);
    send_mouse(QEvent::MouseButtonDblClick, button, button);
    send_mouse(QEvent::MouseButtonRelease, button, Qt::NoButton);
  } else {
    send_mouse(QEvent::MouseButtonPress, button, button);
    send_mouse(QEvent::MouseButtonRelease, button, Qt::NoButton);
  }

  QCoreApplication::processEvents(QEventLoop::AllEvents, 16);
}

void ProbeInputInjector::DeliverMouseDrag(const QPointF& from, const QPointF& to, int steps) {
  if (window_ == nullptr) {
    return;
  }
  auto to_global = [this](const QPointF& scene) {
    return window_->mapToGlobal(scene.toPoint());
  };

  {
    QMouseEvent move(QEvent::MouseMove, from, from, to_global(from), Qt::NoButton, Qt::NoButton,
                     Qt::NoModifier);
    QCoreApplication::sendEvent(window_, &move);
  }
  {
    QMouseEvent press(QEvent::MouseButtonPress, from, from, to_global(from), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window_, &press);
  }
  for (int i = 1; i <= steps; ++i) {
    const qreal   t = static_cast<qreal>(i) / static_cast<qreal>(steps);
    const QPointF pos(from.x() + (to.x() - from.x()) * t, from.y() + (to.y() - from.y()) * t);
    QMouseEvent   move(QEvent::MouseMove, pos, pos, to_global(pos), Qt::NoButton, Qt::LeftButton,
                       Qt::NoModifier);
    QCoreApplication::sendEvent(window_, &move);
  }
  {
    QMouseEvent release(QEvent::MouseButtonRelease, to, to, to_global(to), Qt::LeftButton,
                        Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(window_, &release);
  }
  QCoreApplication::processEvents(QEventLoop::AllEvents, 16);
}

void ProbeInputInjector::DeliverKey(int key, Qt::KeyboardModifiers modifiers, const QString& text) {
  if (window_ == nullptr) {
    return;
  }
  QKeyEvent press(QEvent::KeyPress, key, modifiers, text);
  QKeyEvent release(QEvent::KeyRelease, key, modifiers, text);
  QCoreApplication::sendEvent(window_, &press);
  QCoreApplication::sendEvent(window_, &release);
  QCoreApplication::processEvents(QEventLoop::AllEvents, 16);
}

auto ProbeInputInjector::TopmostItemAt(QQuickItem* root, const QPointF& scene_pos) -> QQuickItem* {
  if (root == nullptr || !IsEffectivelyVisible(root)) {
    return nullptr;
  }
  const QPointF local = root->mapFromScene(scene_pos);
  if (!root->contains(local)) {
    return nullptr;
  }

  const QList<QQuickItem*> children = root->childItems();
  for (auto it = children.crbegin(); it != children.crend(); ++it) {
    QQuickItem* child = *it;
    if (child == nullptr || !IsEffectivelyVisible(child) || child->opacity() <= 0.0) {
      continue;
    }
    if (QQuickItem* found = TopmostItemAt(child, scene_pos); found != nullptr) {
      return found;
    }
  }
  return root;
}

bool ProbeInputInjector::IsEffectivelyVisible(QQuickItem* item) {
  for (QQuickItem* current = item; current != nullptr; current = current->parentItem()) {
    if (!current->isVisible() || current->opacity() <= 0.0) {
      return false;
    }
  }
  return item != nullptr;
}

bool ProbeInputInjector::IsModalOverlayAncestor(QQuickItem* item) {
  for (QQuickItem* current = item; current != nullptr; current = current->parentItem()) {
    const QVariant modal = current->property("modal");
    if (modal.isValid() && modal.toBool()) {
      return true;
    }
    const QByteArray class_name = current->metaObject()->className();
    if (class_name.contains("Popup") || class_name.contains("Dialog") ||
        class_name.contains("Drawer") || class_name.contains("Overlay")) {
      const QVariant popup_modal = current->property("modal");
      if (!popup_modal.isValid() || popup_modal.toBool()) {
        return true;
      }
    }
    const QString object_name = current->objectName();
    if (object_name.contains(QStringLiteral("Modal"), Qt::CaseInsensitive) ||
        object_name.endsWith(QStringLiteral("Dialog")) ||
        object_name.endsWith(QStringLiteral("Overlay"))) {
      if (current->isVisible()) {
        return true;
      }
    }
  }
  return false;
}

bool ProbeInputInjector::IsCoveredByModalOverlay(QQuickItem* target, QQuickItem* topmost) {
  if (target == nullptr || topmost == nullptr) {
    return false;
  }
  for (QQuickItem* current = topmost; current != nullptr; current = current->parentItem()) {
    if (current == target) {
      return false;
    }
  }
  return IsModalOverlayAncestor(topmost);
}

}  // namespace alcedo::ui
