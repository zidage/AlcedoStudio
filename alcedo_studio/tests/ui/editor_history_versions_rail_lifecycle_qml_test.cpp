//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_history_versions_rail_lifecycle_qml_test.cpp
/// @brief Phase 7A R6: closed rail has no list delegates; only the active body
///        loads; scroll restores across panel switches; fold layout is binary.

#include "editor_history_versions_rail_qml_harness.hpp"

#include <QMetaObject>
#include <QtGlobal>

namespace alcedo::ui::test {
namespace {

using rail_harness::Click;
using rail_harness::ProcessEvents;
using rail_harness::RailQmlFixture;

class EditorHistoryVersionsRailLifecycleQmlTest : public RailQmlFixture {};

TEST_F(EditorHistoryVersionsRailLifecycleQmlTest,
       ClosedRailOwnsNoTransactionOrVersionDelegates) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();

  auto* rail = Find(QStringLiteral("editorHistoryVersionsRail"));
  ASSERT_NE(rail, nullptr);
  EXPECT_FALSE(rail->property("panelExpanded").toBool());
  EXPECT_FALSE(rail->property("panelBodyActive").toBool());
  EXPECT_EQ(Find(QStringLiteral("editorHistoryPageBody")), nullptr);
  EXPECT_EQ(Find(QStringLiteral("editorVersionsPageBody")), nullptr);
  EXPECT_TRUE(HistoryCards().isEmpty());
  EXPECT_TRUE(Cards().isEmpty());
  EXPECT_EQ(Find(QStringLiteral("editorHistoryList")), nullptr);
  EXPECT_EQ(Find(QStringLiteral("editorVersionsList")), nullptr);
}

TEST_F(EditorHistoryVersionsRailLifecycleQmlTest,
       SwitchingPanelsDestroysInactiveBodyAndRestoresScroll) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  auto* rail = Find(QStringLiteral("editorHistoryVersionsRail"));
  ASSERT_NE(rail, nullptr);

  // Seed enough Versions that contentY is meaningful.
  constexpr int kFillCount = 18;
  for (int i = 0; i < kFillCount; ++i) {
    controller_.CreateRootVersion(QStringLiteral("LifeFill%1").arg(i));
  }
  ProcessEvents();

  OpenVersionsPage();
  auto* versions_list = Find(QStringLiteral("editorVersionsList"));
  ASSERT_NE(versions_list, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(
      versions_list->property("contentHeight").toReal() > versions_list->height() + 48.0, 2000);

  const qreal target_y = 96.0;
  ASSERT_TRUE(versions_list->setProperty("contentY", target_y));
  ProcessEvents();
  QTRY_VERIFY_WITH_TIMEOUT(versions_list->property("contentY").toReal() > 40.0, 1000);
  const qreal y_before_switch = versions_list->property("contentY").toReal();

  const int creates_before = rail->property("panelBodyCreateCount").toInt();
  const int destroys_before = rail->property("panelBodyDestroyCount").toInt();

  // Switch to History: Versions body must be destroyed (not merely hidden).
  OpenHistoryPage();
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorHistoryPageBody")) != nullptr, 2000);
  EXPECT_EQ(Find(QStringLiteral("editorVersionsPageBody")), nullptr);
  EXPECT_TRUE(Cards().isEmpty());
  EXPECT_FALSE(HistoryCards().isEmpty());
  EXPECT_GE(rail->property("panelBodyDestroyCount").toInt(), destroys_before + 1);
  EXPECT_GE(rail->property("panelBodyCreateCount").toInt(), creates_before + 1);
  EXPECT_NEAR(rail->property("versionsListContentY").toReal(), y_before_switch, 2.0);

  // Return to Versions: prior scroll restores from rail-owned state.
  OpenVersionsPage();
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorVersionsPageBody")) != nullptr, 2000);
  EXPECT_EQ(Find(QStringLiteral("editorHistoryPageBody")), nullptr);
  EXPECT_TRUE(HistoryCards().isEmpty());
  versions_list = Find(QStringLiteral("editorVersionsList"));
  ASSERT_NE(versions_list, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(
      qAbs(versions_list->property("contentY").toReal() - y_before_switch) <= 2.0, 2000);
}

TEST_F(EditorHistoryVersionsRailLifecycleQmlTest,
       CollapsingRailDestroysActiveBodyAndClearsDelegates) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  auto* rail = Find(QStringLiteral("editorHistoryVersionsRail"));
  ASSERT_NE(rail, nullptr);

  OpenHistoryPage();
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorHistoryPageBody")) != nullptr, 2000);
  ASSERT_FALSE(HistoryCards().isEmpty());
  const int destroys_before = rail->property("panelBodyDestroyCount").toInt();

  // Toggle History closed.
  OpenHistoryPage();
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorHistoryPageBody")) == nullptr, 2000);
  EXPECT_FALSE(rail->property("panelBodyActive").toBool());
  EXPECT_TRUE(HistoryCards().isEmpty());
  EXPECT_TRUE(Cards().isEmpty());
  EXPECT_GE(rail->property("panelBodyDestroyCount").toInt(), destroys_before + 1);
}

TEST_F(EditorHistoryVersionsRailLifecycleQmlTest,
       FoldProgressKeepsBinaryOuterLayoutAndTransformOnlySlide) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  auto* rail = Find(QStringLiteral("editorHistoryVersionsRail"));
  ASSERT_NE(rail, nullptr);

  const int rail_width = rail->property("railWidth").toInt();
  const int panel_width = rail->property("expandedPanelWidth").toInt();
  const int panel_gap = rail->property("panelGap").toInt();
  const qreal full_w = rail_width + panel_gap + panel_width;
  ASSERT_GT(rail_width, 0);
  ASSERT_GT(panel_width, 0);

  // Harness Loader stretches the rail Item to the window; assert the layout
  // contract (totalWidth / layoutExpanded / panelSlideX), not scene width.
  ASSERT_TRUE(QMetaObject::invokeMethod(rail, "driveFoldProgress", Q_ARG(QVariant, QVariant(0.0))));
  ProcessEvents();
  EXPECT_NEAR(rail->property("totalWidth").toReal(), rail_width, 1.0);
  EXPECT_FALSE(rail->property("layoutExpanded").toBool());

  ASSERT_TRUE(QMetaObject::invokeMethod(rail, "driveFoldProgress", Q_ARG(QVariant, QVariant(0.5))));
  ProcessEvents();
  // Binary full outer layout width at mid progress (not half-width thrash).
  EXPECT_NEAR(rail->property("totalWidth").toReal(), full_w, 1.5);
  EXPECT_TRUE(rail->property("layoutExpanded").toBool());
  EXPECT_NEAR(rail->property("panelSlideX").toReal(), -0.5 * panel_width, 1.5);

  // Panel shell must not fade the complex subtree (opacity stays fully opaque).
  auto* panel = Find(QStringLiteral("editorHistoryVersionsPanel"));
  ASSERT_NE(panel, nullptr);
  EXPECT_NEAR(panel->opacity(), 1.0, 0.001);

  ASSERT_TRUE(QMetaObject::invokeMethod(rail, "driveFoldProgress", Q_ARG(QVariant, QVariant(1.0))));
  ProcessEvents();
  EXPECT_NEAR(rail->property("totalWidth").toReal(), full_w, 1.5);
  EXPECT_NEAR(rail->property("panelSlideX").toReal(), 0.0, 1.0);

  ASSERT_TRUE(QMetaObject::invokeMethod(rail, "endFoldDrive"));
  ProcessEvents();
}

TEST_F(EditorHistoryVersionsRailLifecycleQmlTest,
       ActiveHistoryAndVersionsListsEnableReuseItems) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();

  OpenHistoryPage();
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorHistoryList")) != nullptr, 2000);
  auto* history_list = Find(QStringLiteral("editorHistoryList"));
  ASSERT_NE(history_list, nullptr);
  EXPECT_TRUE(history_list->property("reuseItems").toBool());

  OpenVersionsPage();
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorVersionsList")) != nullptr, 2000);
  auto* versions_list = Find(QStringLiteral("editorVersionsList"));
  ASSERT_NE(versions_list, nullptr);
  EXPECT_TRUE(versions_list->property("reuseItems").toBool());
  EXPECT_EQ(Find(QStringLiteral("editorHistoryList")), nullptr);
}

}  // namespace
}  // namespace alcedo::ui::test
