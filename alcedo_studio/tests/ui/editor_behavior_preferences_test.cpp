//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/editor_behavior_preferences.hpp"

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>

namespace alcedo::ui::test {
namespace {

class EditorBehaviorPreferencesTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(settings_dir_.isValid());
    QCoreApplication::setOrganizationName("PuerhLabTest");
    QCoreApplication::setApplicationName("EditorBehaviorPreferencesTest");
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir_.path());
    QSettings settings;
    settings.clear();
    settings.sync();
  }

  QTemporaryDir settings_dir_;
};

TEST_F(EditorBehaviorPreferencesTest, LockedNodeMaskActionDefaultsToAsk) {
  EditorBehaviorPreferences preferences;
  EXPECT_EQ(preferences.locked_node_mask_action(), QStringLiteral("ask"));
}

TEST_F(EditorBehaviorPreferencesTest, LockedNodeMaskActionPersistsAcrossInstances) {
  EditorBehaviorPreferences first;
  QSignalSpy changed(&first, &EditorBehaviorPreferences::lockedNodeMaskActionChanged);

  EXPECT_TRUE(first.setLockedNodeMaskAction(QStringLiteral("newLayer")));
  EXPECT_EQ(first.locked_node_mask_action(), QStringLiteral("newLayer"));
  EXPECT_EQ(changed.count(), 1);
  // Re-setting the same value is accepted without another notification.
  EXPECT_TRUE(first.setLockedNodeMaskAction(QStringLiteral("newLayer")));
  EXPECT_EQ(changed.count(), 1);

  EditorBehaviorPreferences second;
  EXPECT_EQ(second.locked_node_mask_action(), QStringLiteral("newLayer"));
  EXPECT_EQ(QSettings{}.value("editor/lockedNodeMaskAction").toString(),
            QStringLiteral("newLayer"));
}

TEST_F(EditorBehaviorPreferencesTest, UnknownLockedNodeMaskActionIsRejectedWithoutChange) {
  EditorBehaviorPreferences preferences;
  ASSERT_TRUE(preferences.setLockedNodeMaskAction(QStringLiteral("currentNode")));
  QSignalSpy changed(&preferences, &EditorBehaviorPreferences::lockedNodeMaskActionChanged);

  EXPECT_FALSE(preferences.setLockedNodeMaskAction(QStringLiteral("cancel")));
  EXPECT_EQ(preferences.locked_node_mask_action(), QStringLiteral("currentNode"));
  EXPECT_EQ(changed.count(), 0);
  EXPECT_EQ(QSettings{}.value("editor/lockedNodeMaskAction").toString(),
            QStringLiteral("currentNode"));
}

TEST_F(EditorBehaviorPreferencesTest, UnknownStoredLockedNodeMaskActionReadsAsAsk) {
  QSettings{}.setValue("editor/lockedNodeMaskAction", QStringLiteral("legacyValue"));
  EditorBehaviorPreferences preferences;
  EXPECT_EQ(preferences.locked_node_mask_action(), QStringLiteral("ask"));
}

}  // namespace
}  // namespace alcedo::ui::test
