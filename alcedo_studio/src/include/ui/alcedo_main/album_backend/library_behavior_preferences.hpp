//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>

namespace alcedo::ui {

/**
 * @brief User-level Library defaults.
 *
 * Owns the default action of Import Folder (#1077), shown in the Import section of the Settings
 * "Default Behavior" page. Import Folder opens a dialog with two choices: Import into Collection
 * and Mount Folder. The action decides whether the dialog asks, or whether Import Folder imports
 * or mounts the folder without the dialog. The "Remember my choice" check box of the dialog
 * writes the same value.
 *
 * Values: "ask" (default), "import", "mount". Stored in QSettings under
 * `library/importFolderAction`; unknown stored values read as "ask".
 *
 * Threading: GUI thread only. Side effects: QSettings write and importFolderActionChanged on an
 * accepted change.
 */
class LibraryBehaviorPreferences final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString importFolderAction READ import_folder_action WRITE setImportFolderAction NOTIFY
                 importFolderActionChanged)
  Q_PROPERTY(bool skipDuplicateImports READ LoadSkipDuplicateImports WRITE setSkipDuplicateImports
                 NOTIFY skipDuplicateImportsChanged)

 public:
  static constexpr auto kImportFolderActionSettingKey = "library/importFolderAction";
  static constexpr auto kAskAction                    = "ask";
  static constexpr auto kImportAction                 = "import";
  static constexpr auto kMountAction                  = "mount";

  explicit LibraryBehaviorPreferences(QObject* parent = nullptr);

  [[nodiscard]] auto import_folder_action() const -> QString { return import_folder_action_; }
  /**
   * @brief Persist @p action as the default action of Import Folder.
   * @return false for values outside ask/import/mount; nothing changes.
   */
  Q_INVOKABLE bool   setImportFolderAction(const QString& action);

  [[nodiscard]] static auto IsImportFolderAction(const QString& action) -> bool;
  [[nodiscard]] static auto LoadSkipDuplicateImports() -> bool;
  Q_INVOKABLE void          setSkipDuplicateImports(bool enabled);

 signals:
  void importFolderActionChanged();
  void skipDuplicateImportsChanged();

 private:
  QString import_folder_action_;
};

}  // namespace alcedo::ui
