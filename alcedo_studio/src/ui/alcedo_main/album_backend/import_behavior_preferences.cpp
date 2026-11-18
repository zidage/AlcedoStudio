// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#include "ui/alcedo_main/album_backend/import_behavior_preferences.hpp"

#include <QSettings>
#include <QString>

namespace alcedo::ui {
ImportBehaviorPreferences::ImportBehaviorPreferences(QObject* parent) : QObject(parent) {
  const auto stored = QSettings{}.value(QLatin1String(kSettingKey));
  // QSettings can serialize booleans as strings. Reject unknown values explicitly.
  const auto text   = stored.toString().toLower();
  if (text == QLatin1String("false")) skip_duplicates_ = false;
}

void ImportBehaviorPreferences::SetSkipDuplicateImports(bool enabled) {
  QSettings{}.setValue(QLatin1String(kSettingKey), enabled);
  if (skip_duplicates_ == enabled) return;
  skip_duplicates_ = enabled;
  emit SkipDuplicateImportsChanged();
}
}  // namespace alcedo::ui
