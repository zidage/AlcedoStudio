//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The default action of Import Folder (#1077): LibraryBehaviorPreferences and its QSettings key.

#include "ui/alcedo_main/album_backend/library_behavior_preferences.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>

namespace alcedo::ui::test {
namespace {

class LibraryBehaviorPreferencesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(settings_dir_.isValid());
    QCoreApplication::setOrganizationName("PuerhLabTest");
    QCoreApplication::setApplicationName("LibraryBehaviorPreferencesTest");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir_.path());
    QSettings settings;
    settings.clear();
    settings.sync();
  }

  QTemporaryDir settings_dir_;
};

TEST_F(LibraryBehaviorPreferencesTest, ImportFolderActionDefaultsToAsk) {
  LibraryBehaviorPreferences preferences;
  EXPECT_EQ(preferences.import_folder_action(), QStringLiteral("ask"));
}

TEST_F(LibraryBehaviorPreferencesTest, EachImportFolderActionPersistsAcrossInstances) {
  for (const auto& action :
       {QStringLiteral("import"), QStringLiteral("mount"), QStringLiteral("ask")}) {
    LibraryBehaviorPreferences first;
    ASSERT_TRUE(first.setImportFolderAction(action));
    EXPECT_EQ(first.import_folder_action(), action);

    LibraryBehaviorPreferences second;
    EXPECT_EQ(second.import_folder_action(), action);
    EXPECT_EQ(QSettings{}.value("library/importFolderAction").toString(), action);
  }
}

TEST_F(LibraryBehaviorPreferencesTest, ImportFolderActionChangeNotifiesOnce) {
  LibraryBehaviorPreferences preferences;
  QSignalSpy changed(&preferences, &LibraryBehaviorPreferences::importFolderActionChanged);

  EXPECT_TRUE(preferences.setImportFolderAction(QStringLiteral("mount")));
  EXPECT_EQ(changed.count(), 1);
  // Setting the same value again is accepted without another notification.
  EXPECT_TRUE(preferences.setImportFolderAction(QStringLiteral("mount")));
  EXPECT_EQ(changed.count(), 1);
}

TEST_F(LibraryBehaviorPreferencesTest, UnknownImportFolderActionIsRejectedWithoutChange) {
  LibraryBehaviorPreferences preferences;
  ASSERT_TRUE(preferences.setImportFolderAction(QStringLiteral("import")));
  QSignalSpy changed(&preferences, &LibraryBehaviorPreferences::importFolderActionChanged);

  EXPECT_FALSE(preferences.setImportFolderAction(QStringLiteral("copy")));
  EXPECT_FALSE(preferences.setImportFolderAction(QString{}));
  EXPECT_EQ(preferences.import_folder_action(), QStringLiteral("import"));
  EXPECT_EQ(changed.count(), 0);
  EXPECT_EQ(QSettings{}.value("library/importFolderAction").toString(), QStringLiteral("import"));
}

TEST_F(LibraryBehaviorPreferencesTest, UnknownStoredImportFolderActionReadsAsAsk) {
  QSettings{}.setValue("library/importFolderAction", QStringLiteral("legacyValue"));
  LibraryBehaviorPreferences preferences;
  EXPECT_EQ(preferences.import_folder_action(), QStringLiteral("ask"));
}

TEST_F(LibraryBehaviorPreferencesTest, DuplicateSkippingDefaultsOnAndPersistsOptOut) {
  LibraryBehaviorPreferences preferences;
  EXPECT_TRUE(preferences.property("skipDuplicateImports").toBool());
  QSignalSpy changed(&preferences, &LibraryBehaviorPreferences::skipDuplicateImportsChanged);
  preferences.setSkipDuplicateImports(false);
  preferences.setSkipDuplicateImports(false);
  EXPECT_EQ(changed.count(), 1);
  LibraryBehaviorPreferences reopened;
  EXPECT_FALSE(reopened.property("skipDuplicateImports").toBool());
  EXPECT_FALSE(QSettings{}.value("import/skipDuplicates", true).toBool());
  reopened.setSkipDuplicateImports(true);
  EXPECT_TRUE(LibraryBehaviorPreferences::LoadSkipDuplicateImports());
}

}  // namespace
}  // namespace alcedo::ui::test
