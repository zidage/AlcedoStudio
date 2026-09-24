//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_behavior_preferences.hpp"

#include <QLatin1String>
#include <QSettings>

namespace alcedo::ui {

EditorBehaviorPreferences::EditorBehaviorPreferences(QObject* parent) : QObject(parent) {
  const QString stored =
      QSettings{}
          .value(QLatin1String(kLockedNodeMaskActionSettingKey), QLatin1String(kAskAction))
          .toString();
  locked_node_mask_action_ =
      IsLockedNodeMaskAction(stored) ? stored : QString::fromLatin1(kAskAction);
}

auto EditorBehaviorPreferences::IsLockedNodeMaskAction(const QString& action) -> bool {
  return action == QLatin1String(kAskAction) || action == QLatin1String(kNewLayerAction) ||
         action == QLatin1String(kCurrentNodeAction);
}

bool EditorBehaviorPreferences::setLockedNodeMaskAction(const QString& action) {
  if (!IsLockedNodeMaskAction(action)) {
    return false;
  }
  if (action == locked_node_mask_action_) {
    return true;
  }
  locked_node_mask_action_ = action;
  QSettings{}.setValue(QLatin1String(kLockedNodeMaskActionSettingKey), locked_node_mask_action_);
  emit lockedNodeMaskActionChanged();
  return true;
}

}  // namespace alcedo::ui
