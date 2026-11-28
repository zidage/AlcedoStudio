//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/library_behavior_preferences.hpp"

#include <QLatin1String>
#include <QSettings>

namespace alcedo::ui {
namespace {
constexpr auto kSkipDuplicateImportsKey = "import/skipDuplicates";
}

LibraryBehaviorPreferences::LibraryBehaviorPreferences(QObject* parent) : QObject(parent) {
  const QString stored =
      QSettings{}
          .value(QLatin1String(kImportFolderActionSettingKey), QLatin1String(kAskAction))
          .toString();
  import_folder_action_ = IsImportFolderAction(stored) ? stored : QString::fromLatin1(kAskAction);
}

auto LibraryBehaviorPreferences::IsImportFolderAction(const QString& action) -> bool {
  return action == QLatin1String(kAskAction) || action == QLatin1String(kImportAction) ||
         action == QLatin1String(kMountAction);
}

bool LibraryBehaviorPreferences::setImportFolderAction(const QString& action) {
  if (!IsImportFolderAction(action)) {
    return false;
  }
  if (action == import_folder_action_) {
    return true;
  }
  import_folder_action_ = action;
  QSettings{}.setValue(QLatin1String(kImportFolderActionSettingKey), import_folder_action_);
  emit importFolderActionChanged();
  return true;
}

auto LibraryBehaviorPreferences::LoadSkipDuplicateImports() -> bool {
  return QSettings{}.value(QLatin1String(kSkipDuplicateImportsKey), true).toBool();
}

void LibraryBehaviorPreferences::setSkipDuplicateImports(bool enabled) {
  if (LoadSkipDuplicateImports() != enabled) {
    QSettings{}.setValue(QLatin1String(kSkipDuplicateImportsKey), enabled);
    emit skipDuplicateImportsChanged();
  }
}

}  // namespace alcedo::ui
