//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>

namespace alcedo::ui {

/**
 * @brief User-level editor defaults shown on the Settings "Default Behavior" page.
 *
 * Owns the persisted choice for adding a Mask while a deletion-protected
 * (locked) Color Grade is selected. The default Color Grade carries the
 * application's default adjustments and is locked, so a Mask drawn there also
 * scopes those defaults. The action decides whether the editor asks, inserts a
 * new layer above the stack first, or draws on the locked node.
 *
 * Values: "ask" (default), "newLayer", "currentNode". Stored in QSettings under
 * `editor/lockedNodeMaskAction`; unknown stored values read as "ask".
 *
 * Threading: GUI thread only. Side effects: QSettings write and
 * lockedNodeMaskActionChanged on an accepted change.
 */
class EditorBehaviorPreferences final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString lockedNodeMaskAction READ locked_node_mask_action WRITE setLockedNodeMaskAction
                 NOTIFY lockedNodeMaskActionChanged)

 public:
  static constexpr auto kLockedNodeMaskActionSettingKey = "editor/lockedNodeMaskAction";
  static constexpr auto kAskAction                      = "ask";
  static constexpr auto kNewLayerAction                 = "newLayer";
  static constexpr auto kCurrentNodeAction              = "currentNode";

  explicit EditorBehaviorPreferences(QObject* parent = nullptr);

  [[nodiscard]] auto locked_node_mask_action() const -> QString { return locked_node_mask_action_; }
  /**
   * @brief Persist @p action for Mask creation on a locked Color Grade.
   * @return false for values outside ask/newLayer/currentNode; nothing changes.
   */
  Q_INVOKABLE bool   setLockedNodeMaskAction(const QString& action);

  [[nodiscard]] static auto IsLockedNodeMaskAction(const QString& action) -> bool;

 signals:
  void lockedNodeMaskActionChanged();

 private:
  QString locked_node_mask_action_;
};

}  // namespace alcedo::ui
