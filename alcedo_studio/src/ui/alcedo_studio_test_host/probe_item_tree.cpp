//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "probe_item_tree.hpp"

#include "probe_json.hpp"

#include <QJsonArray>
#include <QQmlApplicationEngine>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <functional>

namespace alcedo::ui {
namespace {

auto SceneRectForItem(QQuickItem* item) -> QRectF {
  if (item == nullptr) {
    return {};
  }
  if (item->window() != nullptr) {
    return item->mapRectToScene(item->boundingRect());
  }
  return item->boundingRect();
}

}  // namespace

ProbeItemTree::ProbeItemTree(QQmlApplicationEngine* engine, QQuickWindow* window)
    : engine_(engine), window_(window) {}

auto ProbeItemTree::Snapshot() const -> QJsonObject {
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

auto ProbeItemTree::FindTarget(const QString& target) const -> std::optional<TargetMatch> {
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
    const QString name = ProbeItemTree::TargetName(item);
    if (!name.isEmpty()) {
      path.append(name);
    }
    if (item->objectName() == target || ProbeItemTree::TestId(item) == target ||
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

auto ProbeItemTree::SummarizeItem(QQuickItem* item, const QStringList& path) const -> QJsonObject {
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
  result.insert(QStringLiteral("path"), probe_json::JsonPath(path));

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
      result.insert(QString::fromLatin1(property_name), probe_json::VariantToJson(value));
    }
  }
  return result;
}

auto ProbeItemTree::ReadItemProperty(QQuickItem* item, const QString& property) const
    -> std::optional<QVariant> {
  if (item == nullptr || property.isEmpty()) {
    return std::nullopt;
  }
  if (property == QStringLiteral("visible")) {
    return item->isVisible();
  }
  if (property == QStringLiteral("enabled")) {
    return item->isEnabled();
  }
  if (property == QStringLiteral("activeFocus")) {
    return item->hasActiveFocus();
  }
  if (property == QStringLiteral("width")) {
    return item->width();
  }
  if (property == QStringLiteral("height")) {
    return item->height();
  }
  if (property == QStringLiteral("opacity")) {
    return item->opacity();
  }
  const QQmlProperty qml_property(item, property);
  if (qml_property.isValid()) {
    return qml_property.read();
  }
  const QVariant dynamic_value = item->property(property.toUtf8().constData());
  if (dynamic_value.isValid()) {
    return dynamic_value;
  }
  return std::nullopt;
}

auto ProbeItemTree::TargetName(QQuickItem* item) -> QString {
  if (item == nullptr) {
    return {};
  }
  if (!item->objectName().isEmpty()) {
    return item->objectName();
  }
  return TestId(item);
}

auto ProbeItemTree::TestId(QQuickItem* item) -> QString {
  if (item == nullptr) {
    return {};
  }
  const QVariant value = item->property("testId");
  return value.isValid() ? value.toString() : QString();
}

}  // namespace alcedo::ui
