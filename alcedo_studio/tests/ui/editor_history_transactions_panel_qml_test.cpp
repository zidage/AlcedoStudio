//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_history_transactions_panel_qml_test.cpp
/// @brief Production History transactions panel: toolbar, Branch Here, recovery.

#include "editor_history_versions_rail_qml_harness.hpp"

namespace alcedo::ui::test {
namespace {

using rail_harness::Click;
using rail_harness::FindVisualItem;
using rail_harness::ProcessEvents;
using rail_harness::RailQmlFixture;
using rail_harness::RecoveryBarUrl;
using rail_harness::kRecoveryHarnessQml;

class EditorHistoryTransactionsPanelQmlTest : public RailQmlFixture {};

TEST_F(EditorHistoryTransactionsPanelQmlTest,
       SaveRecoveryBarShowsFailureDetailAndRoutesEveryRecoveryAction) {
  backend_.SetRecovery(true, "A materialization failed");

  QQmlApplicationEngine recovery_engine;
  recovery_engine.addImportPath(QStringLiteral("qrc:/"));
  recovery_engine.addImportPath(rail_harness::QmlDirectory());
  recovery_engine.rootContext()->setContextProperty(QStringLiteral("appTheme"),
                                                    &AppTheme::Instance());
  recovery_engine.rootContext()->setContextProperty(QStringLiteral("editorSessionFake"),
                                                    &controller_);
  recovery_engine.rootContext()->setContextProperty(QStringLiteral("recoverySourceUrl"),
                                                    RecoveryBarUrl());
  recovery_engine.loadData(QByteArray{kRecoveryHarnessQml},
                           QUrl(QStringLiteral("file:///EditorSaveRecoveryHarness.qml")));
  ASSERT_FALSE(recovery_engine.rootObjects().empty());
  auto* recovery_window = qobject_cast<QQuickWindow*>(recovery_engine.rootObjects().front());
  ASSERT_NE(recovery_window, nullptr);
  recovery_window->show();
  ProcessEvents();

  auto* recovery_bar =
      FindVisualItem(recovery_window->contentItem(), QStringLiteral("editorSaveRecoveryBar"));
  ASSERT_NE(recovery_bar, nullptr);
  EXPECT_TRUE(recovery_bar->property("visible").toBool());
  auto* detail = FindVisualItem(recovery_bar, QStringLiteral("editorRecoveryDetail"));
  ASSERT_NE(detail, nullptr);
  EXPECT_EQ(detail->property("text").toString(), QStringLiteral("A materialization failed"));

  Click(recovery_window, FindVisualItem(recovery_bar, QStringLiteral("editorRecoveryRetryButton")));
  EXPECT_EQ(backend_.retry_save_count(), 1);
  EXPECT_FALSE(recovery_bar->property("visible").toBool());

  backend_.SetRecovery(true, "Discard this failed checkpoint");
  ProcessEvents();
  Click(recovery_window,
        FindVisualItem(recovery_bar, QStringLiteral("editorRecoveryDiscardButton")));
  EXPECT_EQ(backend_.discard_count(), 1);
  EXPECT_FALSE(recovery_bar->property("visible").toBool());

  backend_.SetRecovery(true, "Cancel this pending navigation");
  ProcessEvents();
  Click(recovery_window,
        FindVisualItem(recovery_bar, QStringLiteral("editorRecoveryCancelButton")));
  EXPECT_EQ(backend_.cancel_recovery_count(), 1);
  EXPECT_FALSE(recovery_bar->property("visible").toBool());
}

TEST_F(EditorHistoryTransactionsPanelQmlTest, HistoryToolbarUndoAndRedoFollowUserClicks) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenHistoryPage();

  auto* undo = Find(QStringLiteral("editorHistoryUndoButton"));
  auto* redo = Find(QStringLiteral("editorHistoryRedoButton"));
  ASSERT_NE(undo, nullptr);
  ASSERT_NE(redo, nullptr);
  ASSERT_TRUE(undo->isEnabled());
  ASSERT_FALSE(redo->isEnabled());
  auto* recovery_notice = Find(QStringLiteral("editorHistoryRecoveryNotice"));
  ASSERT_NE(recovery_notice, nullptr);
  EXPECT_TRUE(recovery_notice->property("visible").toBool());
  auto* merge_title = Find(QStringLiteral("editorHistoryCommitTitle"));
  ASSERT_NE(merge_title, nullptr);
  EXPECT_TRUE(merge_title->property("text").toString().contains(QStringLiteral("second parent")));
  auto* history_card = Find(QStringLiteral("editorHistoryCard"));
  ASSERT_NE(history_card, nullptr);
  EXPECT_EQ(history_card->property("color").value<QColor>(),
            AppTheme::Instance().cardSurfaceColor());
  EXPECT_EQ(history_card->property("selectionOutlineColor").value<QColor>(),
            AppTheme::Instance().textColor());
  EXPECT_EQ(merge_title->property("color").value<QColor>(), AppTheme::Instance().textColor());

  Click(window_, undo);
  EXPECT_EQ(backend_.undo_count(), 1);
  EXPECT_FALSE(Find(QStringLiteral("editorHistoryUndoButton"))->isEnabled());
  EXPECT_TRUE(Find(QStringLiteral("editorHistoryRedoButton"))->isEnabled());

  Click(window_, Find(QStringLiteral("editorHistoryRedoButton")));
  EXPECT_EQ(backend_.redo_count(), 1);
  EXPECT_TRUE(Find(QStringLiteral("editorHistoryUndoButton"))->isEnabled());
  EXPECT_FALSE(Find(QStringLiteral("editorHistoryRedoButton"))->isEnabled());
}

TEST_F(EditorHistoryTransactionsPanelQmlTest, PasteAndMergeUseVisibleActionsAndResolveEveryField) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenHistoryPage();

  Click(window_, Find(QStringLiteral("editorHistoryPasteButton")));
  EXPECT_EQ(transfer_.paste_count(), 1);

  Click(window_, Find(QStringLiteral("editorHistoryMergeButton")));
  EXPECT_EQ(transfer_.begin_merge_count(), 1);
  auto* dialog = window_->findChild<QObject*>(QStringLiteral("editorMergeDialog"));
  ASSERT_NE(dialog, nullptr);
  ASSERT_TRUE(dialog->property("visible").toBool());

  QQuickItem* choice = nullptr;
  QTRY_VERIFY_WITH_TIMEOUT((choice = Find(QStringLiteral("editorMergeConflictChoice"))) != nullptr,
                           1000);
  Click(window_, choice);
  QTest::keyClick(window_, Qt::Key_Down);
  QTest::keyClick(window_, Qt::Key_Return);
  ProcessEvents();

  Click(window_, Find(QStringLiteral("editorMergeAcceptButton")));
  EXPECT_EQ(transfer_.complete_merge_count(), 1);
  EXPECT_EQ(transfer_.last_resolution_count(), 1);
  EXPECT_EQ(transfer_.last_resolution().value(QStringLiteral("resolvedValue")).toDouble(), 1.0);
}

TEST_F(EditorHistoryTransactionsPanelQmlTest,
       HistoryCardClickMovesToCommitAndBranchButtonUsesSelectedCommitId) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenHistoryPage();

  // Wait for a non-current history card to move the working head in one operation.
  auto find_move_delegate = [&]() -> QQuickItem* {
    for (auto* delegate : HistoryCards()) {
      if (!delegate->property("currentTransaction").toBool() &&
          delegate->property("transactionId").toString().length() > 0) {
        return delegate;
      }
    }
    return nullptr;
  };
  QQuickItem* move_target = nullptr;
  QTRY_VERIFY_WITH_TIMEOUT((move_target = find_move_delegate()) != nullptr, 2000);
  const QString move_id = move_target->property("transactionId").toString();
  auto* move_card = move_target->findChild<QQuickItem*>(QStringLiteral("editorHistoryCard"));
  ASSERT_NE(move_card, nullptr);
  Click(window_, move_card);
  EXPECT_EQ(backend_.move_head_count(), 1);
  EXPECT_EQ(QString::fromStdString(backend_.last_move_head_commit().ToString()), move_id);
  // The typed operation result is published at the QML boundary.
  EXPECT_EQ(controller_.last_history_result().value(QStringLiteral("action")).toString(),
            QStringLiteral("moveHeadToCommit"));
  EXPECT_FALSE(controller_.last_history_failed());

  // Branch Here stays on eligible transaction cards (not Version selection rows).
  EXPECT_EQ(Find(QStringLiteral("editorHistoryBranchButton")) != nullptr ||
                [&]() {
                  for (auto* d : HistoryCards()) {
                    if (d->findChild<QQuickItem*>(QStringLiteral("editorHistoryBranchButton")))
                      return true;
                  }
                  return false;
                }(),
            true);
  // No Branch Here control on Version cards.
  OpenVersionsPage();
  for (auto* card : Cards()) {
    EXPECT_EQ(card->findChild<QQuickItem*>(QStringLiteral("editorHistoryBranchButton")), nullptr);
  }
  OpenHistoryPage();

  // After the move, the model resets; wait for a non-current delegate whose Branch Here
  // button is visible and enabled before clicking, so the re-render never races the click.
  auto find_branch_delegate = [&]() -> QQuickItem* {
    for (auto* delegate : HistoryCards()) {
      if (delegate->property("currentTransaction").toBool()) continue;
      if (delegate->property("transactionId").toString().length() == 0) continue;
      auto* btn = delegate->findChild<QQuickItem*>(QStringLiteral("editorHistoryBranchButton"));
      if (btn != nullptr && btn->property("visible").toBool() && btn->isEnabled()) {
        return delegate;
      }
    }
    return nullptr;
  };
  QQuickItem* branch_target = nullptr;
  QTRY_VERIFY_WITH_TIMEOUT((branch_target = find_branch_delegate()) != nullptr, 2000);
  const QString branch_id = branch_target->property("transactionId").toString();
  auto* branch_button =
      branch_target->findChild<QQuickItem*>(QStringLiteral("editorHistoryBranchButton"));
  ASSERT_NE(branch_button, nullptr);
  // Emit the button's clicked signal directly; this triggers the same onClicked
  // handler as a mouse press without the flaky delegate-geometry race.
  QVERIFY(QMetaObject::invokeMethod(branch_button, "clicked", Qt::DirectConnection));
  ProcessEvents();
  EXPECT_EQ(backend_.branch_count(), 1);
  EXPECT_EQ(QString::fromStdString(backend_.last_branch_commit().ToString()), branch_id);
  EXPECT_EQ(controller_.last_history_result().value(QStringLiteral("action")).toString(),
            QStringLiteral("branchFromCommit"));
}

TEST_F(EditorHistoryTransactionsPanelQmlTest,
       NonCurrentCardsRenderBeforeAfterValueText) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenHistoryPage();
  // Fixture seeds merge(Current) + contrast(Applied) + exposure(Applied). Every
  // non-current edit card must render its before/after value line, not only the
  // head. Regression: the value label previously stayed empty for non-current
  // cards because the JS-block text binding did not re-evaluate; it now binds
  // directly to the authoritative delta_text the presentation helper computes.
  for (auto* delegate : HistoryCards()) {
    if (delegate->property("mergeTransaction").toBool()) continue;
    if (delegate->property("currentTransaction").toBool()) continue;
    auto* value_label =
        delegate->findChild<QQuickItem*>(QStringLiteral("editorHistoryCommitValue"));
    ASSERT_NE(value_label, nullptr);
    const QString text = value_label->property("text").toString();
    EXPECT_FALSE(text.isEmpty()) << "non-current edit card missing before/after value text";
    const QString name = delegate->property("transactionDisplayName").toString();
    if (name == QStringLiteral("Contrast")) {
      EXPECT_EQ(text, QStringLiteral("0 \u2192 +12")) << "contrast card value line";
    } else if (name == QStringLiteral("Exposure")) {
      EXPECT_EQ(text, QStringLiteral("0 \u2192 +0.35")) << "exposure card value line";
    }
  }
}

}  // namespace
}  // namespace alcedo::ui::test
