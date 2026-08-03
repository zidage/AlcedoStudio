//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QJsonObject>
#include <QPointF>
#include <QString>
#include <Qt>
#include <optional>

class QQuickItem;
class QQuickWindow;

namespace alcedo::ui {

class ProbeItemTree;

/// Real-path mouse/key injection and window screenshot capture for the probe.
///
/// Holds a non-owning window pointer and resolves click targets through
/// @p ProbeItemTree. Synthesizes QMouseEvent / QKeyEvent through QQuickWindow
/// dispatch so hover, focus, and pointer-handler semantics match user input.
class ProbeInputInjector final {
 public:
  enum class ClickKind { Single, Double, Right };

  ProbeInputInjector(ProbeItemTree* tree, QQuickWindow* window);

  [[nodiscard]] auto HandleClick(const QJsonObject& request, ClickKind kind) -> QJsonObject;
  [[nodiscard]] auto HandleKey(const QJsonObject& request) -> QJsonObject;
  [[nodiscard]] auto HandleTypeText(const QJsonObject& request) -> QJsonObject;
  [[nodiscard]] auto HandleDrag(const QJsonObject& request) -> QJsonObject;
  [[nodiscard]] auto HandleScreenshot(const QJsonObject& request) -> QJsonObject;

 private:
  [[nodiscard]] auto ValidateClickable(QQuickItem* item) const -> std::optional<QString>;
  [[nodiscard]] auto SceneCenter(QQuickItem* item) const -> QPointF;
  void               DeliverMouseClick(const QPointF& scene_pos, Qt::MouseButton button,
                                       bool double_click);
  void               DeliverMouseDrag(const QPointF& from, const QPointF& to, int steps);
  void               DeliverKey(int key, Qt::KeyboardModifiers modifiers, const QString& text);

  [[nodiscard]] static auto TopmostItemAt(QQuickItem* root, const QPointF& scene_pos)
      -> QQuickItem*;
  [[nodiscard]] static bool IsEffectivelyVisible(QQuickItem* item);
  [[nodiscard]] static bool IsModalOverlayAncestor(QQuickItem* item);
  [[nodiscard]] static bool IsCoveredByModalOverlay(QQuickItem* target, QQuickItem* topmost);

  ProbeItemTree* tree_   = nullptr;
  QQuickWindow*  window_ = nullptr;
};

}  // namespace alcedo::ui
