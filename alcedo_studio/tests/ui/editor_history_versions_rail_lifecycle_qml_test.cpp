//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_history_versions_rail_lifecycle_qml_test.cpp
/// @brief Closed rail has no list delegates; only the active body loads; scroll
///        restores across panel switches; fold layout interpolates like the
///        filmstrip so the viewport moves with the panel.

#include "editor_history_versions_rail_qml_harness.hpp"

#include <QMetaObject>
#include <QRectF>
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

  auto* rail = Find(QStringLiteral("editorWorkspaceRail"));
  ASSERT_NE(rail, nullptr);
  EXPECT_FALSE(rail->property("panelExpanded").toBool());
  EXPECT_FALSE(rail->property("panelBodyActive").toBool());
  EXPECT_EQ(Find(QStringLiteral("editorHistoryPageBody")), nullptr);
  EXPECT_EQ(Find(QStringLiteral("editorVersionsPageBody")), nullptr);
  EXPECT_EQ(Find(QStringLiteral("editorNodesPageBody")), nullptr);
  EXPECT_TRUE(HistoryCards().isEmpty());
  EXPECT_TRUE(Cards().isEmpty());
  EXPECT_EQ(Find(QStringLiteral("editorHistoryList")), nullptr);
  EXPECT_EQ(Find(QStringLiteral("editorVersionsList")), nullptr);
}

TEST_F(EditorHistoryVersionsRailLifecycleQmlTest,
       SwitchingPanelsDestroysInactiveBodyAndRestoresScroll) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  auto* rail = Find(QStringLiteral("editorWorkspaceRail"));
  ASSERT_NE(rail, nullptr);

  // Seed enough Versions that contentY is meaningful.
  constexpr int kFillCount = 18;
  for (int i = 0; i < kFillCount; ++i) {
    controller_.CreateRootVersion(QStringLiteral("LifeFill%1").arg(i));
  }
  ProcessEvents();

  OpenVersionsPage();
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorVersionsList")) != nullptr, 2000);
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
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorVersionsPageBody")) == nullptr, 2000);
  EXPECT_TRUE(Cards().isEmpty());
  EXPECT_FALSE(HistoryCards().isEmpty());
  EXPECT_GE(rail->property("panelBodyDestroyCount").toInt(), destroys_before + 1);
  EXPECT_GE(rail->property("panelBodyCreateCount").toInt(), creates_before + 1);
  EXPECT_NEAR(rail->property("versionsListContentY").toReal(), y_before_switch, 2.0);

  // Return to Versions: prior scroll restores from rail-owned state.
  OpenVersionsPage();
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorVersionsPageBody")) != nullptr, 2000);
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorHistoryPageBody")) == nullptr, 2000);
  EXPECT_TRUE(HistoryCards().isEmpty());
  versions_list = Find(QStringLiteral("editorVersionsList"));
  ASSERT_NE(versions_list, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(
      qAbs(versions_list->property("contentY").toReal() - y_before_switch) <= 2.0, 2000);
}

TEST_F(EditorHistoryVersionsRailLifecycleQmlTest,
       CollapsingRailDestroysActiveBodyAndClearsDelegates) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  auto* rail = Find(QStringLiteral("editorWorkspaceRail"));
  ASSERT_NE(rail, nullptr);

  OpenHistoryPage();
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorHistoryPageBody")) != nullptr, 2000);
  ASSERT_FALSE(HistoryCards().isEmpty());
  const int destroys_before = rail->property("panelBodyDestroyCount").toInt();

  // Close the History page.
  controller_.set_editor_tool_panel_page(QString());
  ProcessEvents();
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorHistoryPageBody")) == nullptr, 2000);
  EXPECT_FALSE(rail->property("panelBodyActive").toBool());
  EXPECT_TRUE(HistoryCards().isEmpty());
  EXPECT_TRUE(Cards().isEmpty());
  EXPECT_GE(rail->property("panelBodyDestroyCount").toInt(), destroys_before + 1);
}

TEST_F(EditorHistoryVersionsRailLifecycleQmlTest,
       FoldProgressInterpolatesOuterLayoutWidthLikeFilmstrip) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  auto* rail = Find(QStringLiteral("editorWorkspaceRail"));
  ASSERT_NE(rail, nullptr);

  const int rail_width = rail->property("railWidth").toInt();
  const int panel_width = rail->property("expandedPanelWidth").toInt();
  const int panel_gap = rail->property("panelGap").toInt();
  const qreal full_w = rail_width + panel_gap + panel_width;
  const qreal mid_w  = rail_width + 0.5 * (panel_gap + panel_width);
  ASSERT_GT(rail_width, 0);
  ASSERT_GT(panel_width, 0);

  // Harness Loader stretches the rail Item to the window; assert the layout
  // contract (totalWidth / layoutExpanded), not scene width.
  ASSERT_TRUE(QMetaObject::invokeMethod(rail, "driveFoldProgress", Q_ARG(QVariant, QVariant(0.0))));
  ProcessEvents();
  EXPECT_NEAR(rail->property("totalWidth").toReal(), rail_width, 1.0);
  EXPECT_FALSE(rail->property("layoutExpanded").toBool());

  ASSERT_TRUE(QMetaObject::invokeMethod(rail, "driveFoldProgress", Q_ARG(QVariant, QVariant(0.5))));
  ProcessEvents();
  EXPECT_NEAR(rail->property("totalWidth").toReal(), mid_w, 1.5);
  EXPECT_TRUE(rail->property("layoutExpanded").toBool());

  auto* host = Find(QStringLiteral("editorToolPanelHost"));
  ASSERT_NE(host, nullptr);
  EXPECT_NEAR(host->opacity(), 0.5, 0.001);

  ASSERT_TRUE(QMetaObject::invokeMethod(rail, "driveFoldProgress", Q_ARG(QVariant, QVariant(1.0))));
  ProcessEvents();
  EXPECT_NEAR(rail->property("totalWidth").toReal(), full_w, 1.5);
  EXPECT_NEAR(host->opacity(), 1.0, 0.001);

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
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorHistoryList")) == nullptr, 2000);
}

TEST_F(EditorHistoryVersionsRailLifecycleQmlTest, RailButtonsShowFunctionLabelBelowIcon) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  auto* tool_rail = Find(QStringLiteral("editorToolRail"));
  ASSERT_NE(tool_rail, nullptr);

  for (const char* name :
       {"editorHistoryRailButton", "editorVersionsRailButton", "editorNodesRailButton",
        "editorMaskGroupsRailButton", "editorBackgroundTasksRailButton"}) {
    auto* button = Find(QString::fromUtf8(name));
    ASSERT_NE(button, nullptr) << name;
    EXPECT_FALSE(button->property("label").toString().isEmpty()) << name;
    EXPECT_FALSE(button->property("actionName").toString().isEmpty()) << name;
    EXPECT_TRUE(button->activeFocusOnTab()) << name;
    EXPECT_GE(button->height(), AppTheme::Instance().iconButtonHitSizeCompact() - 0.5) << name;
    EXPECT_EQ(button->property("opticalSize").toInt(),
              AppTheme::Instance().iconOpticalSizeCompact())
        << name;

    // The tile stays inside the rail.
    const QRectF button_rect =
        button->mapRectToItem(tool_rail, QRectF(0, 0, button->width(), button->height()));
    EXPECT_GE(button_rect.left(), -0.5) << name;
    EXPECT_LE(button_rect.right(), tool_rail->width() + 0.5) << name;

    // The function label sits under the icon, inside the tile, and is not elided.
    auto* icon  = button->property("iconItem").value<QQuickItem*>();
    auto* label = button->property("labelItem").value<QQuickItem*>();
    ASSERT_NE(icon, nullptr) << name;
    ASSERT_NE(label, nullptr) << name;
    const QRectF icon_rect = icon->mapRectToItem(button, QRectF(0, 0, icon->width(), icon->height()));
    const QRectF label_rect =
        label->mapRectToItem(button, QRectF(0, 0, label->width(), label->height()));
    EXPECT_GE(label_rect.top(), icon_rect.bottom() - 0.5) << name;
    EXPECT_LE(label_rect.bottom(), button->height() + 0.5) << name;
    EXPECT_GT(label->property("contentWidth").toReal(), 0.0) << name;
    EXPECT_FALSE(label->property("truncated").toBool()) << name << " label is elided";
  }

  // Selection is reflected on the labeled tile.
  auto* versions_button = Find(QStringLiteral("editorVersionsRailButton"));
  ASSERT_NE(versions_button, nullptr);
  EXPECT_FALSE(versions_button->property("selected").toBool());
  OpenVersionsPage();
  QTRY_VERIFY_WITH_TIMEOUT(versions_button->property("selected").toBool(), 2000);
  for (const auto& warning : warnings_) {
    EXPECT_FALSE(warning.contains(QStringLiteral("EditorRailButton.qml")) ||
                 warning.contains(QStringLiteral("EditorWorkspaceRail.qml")))
        << warning.toStdString();
  }
}

TEST_F(EditorHistoryVersionsRailLifecycleQmlTest,
       LoadingNodesPageHidesTheGraphAndCloseReleasesDelegates) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenNodesPage();
  auto* nodes = window_->findChild<EditorNodeController*>();
  ASSERT_NE(nodes, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(nodes->has_snapshot(), 2000);
  auto* view = Find(QStringLiteral("editorNodesGraphView"));
  ASSERT_NE(view, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(view->isVisible(), 2000);

  backend_.SetSessionState(alcedo::EditorSessionState::Loading, true);
  ProcessEvents();
  QTRY_VERIFY_WITH_TIMEOUT(!nodes->has_snapshot(), 2000);
  QTRY_VERIFY_WITH_TIMEOUT(!view->isVisible(), 2000);
  auto* loading = Find(QStringLiteral("editorNodesLoadingState"));
  ASSERT_NE(loading, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(loading->isVisible(), 2000);
  EXPECT_EQ(loading->property("text").toString(), QStringLiteral("Loading node graph"));

  controller_.set_editor_tool_panel_page(QString());
  ProcessEvents();
  QTRY_VERIFY_WITH_TIMEOUT(Find(QStringLiteral("editorNodesPageBody")) == nullptr, 2000);
  EXPECT_EQ(Find(QStringLiteral("qan::NodeItem")), nullptr);
}

}  // namespace
}  // namespace alcedo::ui::test
