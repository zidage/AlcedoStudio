// Copyright 2026 Yurun Zi
// SPDX-License-Identifier: GPL-3.0-only
#include "ui/alcedo_main/album_backend/import_behavior_preferences.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>

namespace alcedo::ui {
class ImportBehaviorPreferencesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(settings_.isValid());
    QCoreApplication::setOrganizationName("AlcedoTest");
    QCoreApplication::setApplicationName("ImportBehaviorPreferencesTest");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_.path());
    QSettings{}.clear();
  }
  QTemporaryDir settings_;
};
TEST_F(ImportBehaviorPreferencesTest, MissingImportPreferenceDefaultsToTrue) {
  ImportBehaviorPreferences preferences;
  EXPECT_TRUE(preferences.SkipDuplicateImports());
}
TEST_F(ImportBehaviorPreferencesTest, ApplyPersistsTheValueAndNotifiesOnlyOnChange) {
  ImportBehaviorPreferences preferences;
  QSignalSpy changed(&preferences, &ImportBehaviorPreferences::SkipDuplicateImportsChanged);
  preferences.SetSkipDuplicateImports(false);
  preferences.SetSkipDuplicateImports(false);
  EXPECT_EQ(changed.count(), 1);
  EXPECT_FALSE(ImportBehaviorPreferences{}.SkipDuplicateImports());
  EXPECT_FALSE(QSettings{}.value(ImportBehaviorPreferences::kSettingKey).toBool());
}
TEST_F(ImportBehaviorPreferencesTest, InvalidStoredValueUsesTheTrueDefault) {
  QSettings{}.setValue(ImportBehaviorPreferences::kSettingKey, "invalid");
  EXPECT_TRUE(ImportBehaviorPreferences{}.SkipDuplicateImports());
}
}  // namespace alcedo::ui
