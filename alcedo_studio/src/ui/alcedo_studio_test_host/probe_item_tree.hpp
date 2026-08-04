//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QJsonObject>
#include <QStringList>
#include <QVariant>
#include <optional>

class QQmlApplicationEngine;
class QQuickItem;
class QQuickWindow;

namespace alcedo::ui {

/// Live QQuickItem tree observation for the test probe.
///
/// Owns only non-owning pointers to the engine and window supplied by the host.
/// Resolves targets by objectName / testId / dotted path on every call so
/// Loader-hosted or destroyed items fail loudly instead of retaining stale
/// pointers.
class ProbeItemTree final {
 public:
  struct TargetMatch {
    QQuickItem* item = nullptr;
    QStringList path;
  };

  ProbeItemTree(QQmlApplicationEngine* engine, QQuickWindow* window);

  [[nodiscard]] auto window() const -> QQuickWindow* { return window_; }
  [[nodiscard]] auto engine() const -> QQmlApplicationEngine* { return engine_; }

  [[nodiscard]] auto Snapshot() const -> QJsonObject;
  [[nodiscard]] auto FindTarget(const QString& target) const -> std::optional<TargetMatch>;
  [[nodiscard]] auto SummarizeItem(QQuickItem* item, const QStringList& path) const -> QJsonObject;
  [[nodiscard]] auto ReadItemProperty(QQuickItem* item, const QString& property) const
      -> std::optional<QVariant>;

  [[nodiscard]] static auto TargetName(QQuickItem* item) -> QString;
  [[nodiscard]] static auto TestId(QQuickItem* item) -> QString;

 private:
  QQmlApplicationEngine* engine_ = nullptr;
  QQuickWindow*          window_ = nullptr;
};

}  // namespace alcedo::ui
