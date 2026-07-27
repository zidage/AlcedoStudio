//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_versions_panel_qml_test.cpp
/// @brief Production Versions panel: inline draft, outline selection, trash remove.

#include "editor_history_versions_rail_qml_harness.hpp"

#include <QMetaObject>
#include <QtGlobal>

namespace alcedo::ui::test {
namespace {

using rail_harness::Click;
using rail_harness::ProcessEvents;
using rail_harness::RailQmlFixture;
using rail_harness::StableId;
using rail_harness::TypeText;

class EditorVersionsPanelQmlTest : public RailQmlFixture {};

TEST_F(EditorVersionsPanelQmlTest, ClickingNamedVersionChecksOutStableVersionId) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();

  OpenVersionsPage();
  auto cards = Cards();
  ASSERT_EQ(cards.size(), 2);
  const auto  selected_outline = AppTheme::Instance().textColor();
  QQuickItem* selected_card    = nullptr;
  QQuickItem* alternate_card   = nullptr;
  for (auto* card : cards) {
    if (card->property("versionActive").toBool()) {
      selected_card = card;
    } else {
      alternate_card = card;
    }
  }
  ASSERT_NE(selected_card, nullptr);
  ASSERT_NE(alternate_card, nullptr);
  EXPECT_EQ(selected_card->property("color").value<QColor>(),
            AppTheme::Instance().cardSurfaceColor());
  EXPECT_EQ(selected_card->property("selectionOutlineColor").value<QColor>(), selected_outline);
  ASSERT_NE(selected_card->findChild<QQuickItem*>(QStringLiteral("editorVersionTitle")), nullptr);
  EXPECT_EQ(selected_card->findChild<QQuickItem*>(QStringLiteral("editorVersionTitle"))
                ->property("color")
                .value<QColor>(),
            selected_outline);
  auto* versions_button = Find(QStringLiteral("editorVersionsRailButton"));
  ASSERT_NE(versions_button, nullptr);
  EXPECT_TRUE(versions_button->property("selectedOutline").toBool());
  EXPECT_EQ(versions_button->property("fillSelected").value<QColor>(),
            AppTheme::Instance().cardSurfaceColor());
  const QString alternate_id = alternate_card->property("versionId").toString();
  ASSERT_FALSE(alternate_id.isEmpty());

  Click(window_, alternate_card, QPointF(12.0, alternate_card->height() / 2.0));

  EXPECT_EQ(backend_.checkout_count(), 1);
  EXPECT_EQ(QString::fromStdString(backend_.last_checkout_id().ToString()), alternate_id);
  EXPECT_EQ(controller_.history_snapshot().active_version_id,
            Hash128::FromString(alternate_id.toStdString()));
}

TEST_F(EditorVersionsPanelQmlTest, VersionNameInputCreatesRenamesAndRemovesNamedVersion) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenVersionsPage();

  Click(window_, Find(QStringLiteral("editorCreateVersionButton")));
  auto* field = Find(QStringLiteral("editorVersionNameField"));
  ASSERT_NE(field, nullptr);
  EXPECT_TRUE(field->property("visible").toBool());
  // Modal naming dialog must not exist on the inline path.
  EXPECT_EQ(Find(QStringLiteral("editorVersionNameDialog")), nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(field->hasActiveFocus() || field->property("activeFocus").toBool(),
                           1000);
  // Drive the production TextField text through the real focus/type path, then
  // confirm with the Accept affordance (not only Enter).
  TypeText(window_, QStringLiteral("newlook"));
  EXPECT_TRUE(field->property("text").toString().contains(QStringLiteral("newlook")));
  auto* accept = Find(QStringLiteral("editorVersionAcceptButton"));
  ASSERT_NE(accept, nullptr);
  Click(window_, accept);
  QTRY_VERIFY_WITH_TIMEOUT(backend_.create_count() == 1, 2000);

  const QString created_id = QString::fromStdString(backend_.last_created_id().ToString());
  auto          cards      = Cards();
  ASSERT_EQ(cards.size(), 3);
  QQuickItem* created_card = nullptr;
  for (auto* card : cards) {
    if (card->property("versionId").toString() == created_id) created_card = card;
  }
  ASSERT_NE(created_card, nullptr);
  EXPECT_EQ(created_card->property("displayName").toString(), QStringLiteral("newlook"));
  EXPECT_TRUE(created_card->property("versionActive").toBool());
  EXPECT_FALSE(controller_.history_snapshot().active_head.has_value());

  Click(window_, created_card->findChild<QQuickItem*>(QStringLiteral("editorRenameVersionButton")));
  field = Find(QStringLiteral("editorVersionNameField"));
  ASSERT_NE(field, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(field->hasActiveFocus() || field->property("activeFocus").toBool(),
                           1000);
  TypeText(window_, QStringLiteral("renamedlook"));
  accept = Find(QStringLiteral("editorVersionAcceptButton"));
  ASSERT_NE(accept, nullptr);
  Click(window_, accept);
  QTRY_VERIFY_WITH_TIMEOUT(backend_.rename_count() == 1, 2000);
  EXPECT_EQ(QString::fromStdString(backend_.last_rename_id().ToString()), created_id);

  cards        = Cards();
  created_card = nullptr;
  for (auto* card : cards) {
    if (card->property("versionId").toString() == created_id) created_card = card;
  }
  ASSERT_NE(created_card, nullptr);
  QQuickItem* removable_card = nullptr;
  for (auto* card : cards) {
    if (card->property("versionId").toString() != created_id) removable_card = card;
  }
  ASSERT_NE(removable_card, nullptr);
  Click(window_,
        removable_card->findChild<QQuickItem*>(QStringLiteral("editorRemoveVersionButton")));
  EXPECT_EQ(backend_.remove_count(), 1);
  EXPECT_EQ(backend_.last_removed_id(), StableId(2));
  EXPECT_EQ(Cards().size(), 2);
}

TEST_F(EditorVersionsPanelQmlTest, InlineVersionDraftAcceptsEnterAndEscapeWithoutDialog) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenVersionsPage();

  // No modal naming Dialog in production Versions QML.
  EXPECT_EQ(window_->findChild<QObject*>(QStringLiteral("editorVersionNameDialog")), nullptr);
  EXPECT_EQ(Find(QStringLiteral("editorVersionNameDialog")), nullptr);

  Click(window_, Find(QStringLiteral("editorCreateVersionButton")));
  auto* draft_row = Find(QStringLiteral("editorVersionDraftRow"));
  auto* field     = Find(QStringLiteral("editorVersionNameField"));
  ASSERT_NE(draft_row, nullptr);
  ASSERT_NE(field, nullptr);
  EXPECT_TRUE(draft_row->property("visible").toBool());
  EXPECT_TRUE(field->property("visible").toBool());
  EXPECT_TRUE(field->isEnabled());

  // Generated unique name is pre-filled and selected for overwrite.
  const QString generated = field->property("text").toString();
  EXPECT_FALSE(generated.trimmed().isEmpty());
  EXPECT_TRUE(generated.contains(QStringLiteral("Version"), Qt::CaseInsensitive));
  EXPECT_TRUE(field->hasActiveFocus() || field->property("activeFocus").toBool());

  // Escape cancels without create.
  auto* page = Find(QStringLiteral("editorVersionsPageBody"));
  ASSERT_NE(page, nullptr);
  QTest::keyClick(window_, Qt::Key_Escape);
  ProcessEvents();
  EXPECT_EQ(backend_.create_count(), 0);
  EXPECT_FALSE(page->property("draftVisible").toBool());

  // Re-open, type a name, Enter creates via the real model path.
  Click(window_, Find(QStringLiteral("editorCreateVersionButton")));
  field = Find(QStringLiteral("editorVersionNameField"));
  ASSERT_NE(field, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(field->property("visible").toBool(), 1000);
  TypeText(window_, QStringLiteral("enterlook"));
  QTest::keyClick(window_, Qt::Key_Return);
  ProcessEvents();
  ASSERT_EQ(backend_.create_count(), 1);
  EXPECT_EQ(controller_.last_history_result().value(QStringLiteral("action")).toString(),
            QStringLiteral("createRootVersion"));
  const QString created_id = QString::fromStdString(backend_.last_created_id().ToString());
  QQuickItem*   created    = nullptr;
  for (auto* card : Cards()) {
    if (card->property("versionId").toString() == created_id) created = card;
  }
  ASSERT_NE(created, nullptr);
  EXPECT_EQ(created->property("displayName").toString(), QStringLiteral("enterlook"));
  EXPECT_TRUE(created->property("versionActive").toBool());
  EXPECT_FALSE(page->property("draftVisible").toBool());
  EXPECT_EQ(Find(QStringLiteral("editorVersionNameDialog")), nullptr);
}

TEST_F(EditorVersionsPanelQmlTest, ActiveVersionUsesOutlineWithoutStopPlaybackAction) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenVersionsPage();

  auto cards = Cards();
  ASSERT_GE(cards.size(), 2);
  QQuickItem* active_card     = nullptr;
  QQuickItem* inactive_card   = nullptr;
  for (auto* card : cards) {
    if (card->property("versionActive").toBool()) {
      active_card = card;
    } else {
      inactive_card = card;
    }
  }
  ASSERT_NE(active_card, nullptr);
  ASSERT_NE(inactive_card, nullptr);

  // Outline-only: card surface stays cardSurface; border/selectionOutline is text color.
  EXPECT_EQ(active_card->property("color").value<QColor>(), AppTheme::Instance().cardSurfaceColor());
  EXPECT_EQ(active_card->property("selectionOutlineColor").value<QColor>(),
            AppTheme::Instance().textColor());
  // Inactive uses the quieter card border, not a filled invert well.
  EXPECT_EQ(inactive_card->property("color").value<QColor>(),
            AppTheme::Instance().cardSurfaceColor());
  EXPECT_EQ(inactive_card->property("selectionOutlineColor").value<QColor>(),
            AppTheme::Instance().cardBorderColor());
  // Not a list-invert fill selection (selected well is bone, not card surface).
  EXPECT_NE(active_card->property("color").value<QColor>(),
            AppTheme::Instance().editorListSelectedFillColor());

  auto* remove_btn =
      inactive_card->findChild<QQuickItem*>(QStringLiteral("editorRemoveVersionButton"));
  ASSERT_NE(remove_btn, nullptr);
  const QString icon_src = remove_btn->property("iconSrc").toString();
  EXPECT_FALSE(icon_src.contains(QStringLiteral("stop.svg")));
  EXPECT_TRUE(icon_src.contains(QStringLiteral("trash.svg")));
  const QString action_name = remove_btn->property("actionName").toString();
  EXPECT_TRUE(action_name.contains(QStringLiteral("Remove"), Qt::CaseInsensitive) ||
              action_name.contains(QStringLiteral("Delete"), Qt::CaseInsensitive));
  EXPECT_FALSE(action_name.contains(QStringLiteral("Stop"), Qt::CaseInsensitive));
  EXPECT_FALSE(action_name.contains(QStringLiteral("Play"), Qt::CaseInsensitive));
  EXPECT_FALSE(action_name.contains(QStringLiteral("Playback"), Qt::CaseInsensitive));
  // Accessible name matches remove/delete, not stop/playback.
  EXPECT_EQ(remove_btn->property("Accessible.name").toString().isEmpty()
                ? action_name
                : remove_btn->property("Accessible.name").toString(),
            action_name);
}

TEST_F(EditorVersionsPanelQmlTest, InlineDraftPendingSubmitBlocksDuplicateCreate) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenVersionsPage();

  auto* page = Find(QStringLiteral("editorVersionsPageBody"));
  ASSERT_NE(page, nullptr);

  Click(window_, Find(QStringLiteral("editorCreateVersionButton")));
  auto* field = Find(QStringLiteral("editorVersionNameField"));
  ASSERT_NE(field, nullptr);
  TypeText(window_, QStringLiteral("pendinglook"));

  // Simulate a pending-save lock without closing the draft: force the property
  // then attempt a second accept. The field must stay and create must not fire.
  ASSERT_TRUE(page->setProperty("draftSubmitPending", true));
  ProcessEvents();
  EXPECT_TRUE(page->property("draftVisible").toBool());
  EXPECT_FALSE(field->isEnabled());
  auto* accept = Find(QStringLiteral("editorVersionAcceptButton"));
  ASSERT_NE(accept, nullptr);
  EXPECT_FALSE(accept->isEnabled());

  // Direct re-commit while pending must not reach the backend.
  QMetaObject::invokeMethod(page, "commitDraft", Qt::DirectConnection, Q_ARG(bool, false));
  ProcessEvents();
  EXPECT_EQ(backend_.create_count(), 0);
  EXPECT_TRUE(page->property("draftVisible").toBool());

  // Clear pending and accept for real.
  ASSERT_TRUE(page->setProperty("draftSubmitPending", false));
  ProcessEvents();
  // Re-enable the accept affordance after clearing the lock.
  accept = Find(QStringLiteral("editorVersionAcceptButton"));
  ASSERT_NE(accept, nullptr);
  Click(window_, accept);
  EXPECT_EQ(backend_.create_count(), 1);
}

TEST_F(EditorVersionsPanelQmlTest, RenameUsesSameInlineDraftField) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenVersionsPage();

  auto cards = Cards();
  ASSERT_FALSE(cards.isEmpty());
  QQuickItem* active = nullptr;
  for (auto* card : cards) {
    if (card->property("versionActive").toBool()) active = card;
  }
  ASSERT_NE(active, nullptr);
  Click(window_, active->findChild<QQuickItem*>(QStringLiteral("editorRenameVersionButton")));

  auto* field = Find(QStringLiteral("editorVersionNameField"));
  ASSERT_NE(field, nullptr);
  EXPECT_EQ(Find(QStringLiteral("editorVersionNameDialog")), nullptr);
  EXPECT_EQ(field->property("text").toString(), active->property("displayName").toString());
  TypeText(window_, QStringLiteral("renamedbase"));
  QTest::keyClick(window_, Qt::Key_Return);
  ProcessEvents();
  EXPECT_EQ(backend_.rename_count(), 1);
  EXPECT_EQ(controller_.last_history_result().value(QStringLiteral("action")).toString(),
            QStringLiteral("renameVersion"));
}

TEST_F(EditorVersionsPanelQmlTest,
       VersionListPreservesContentYAcrossCreateRenameAndCheckout) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenVersionsPage();

  // Seed enough named Versions that the list content exceeds the viewport so a
  // non-zero contentY is meaningful (drives real ListView + model reset path).
  constexpr int kFillCount = 18;
  for (int i = 0; i < kFillCount; ++i) {
    controller_.CreateRootVersion(QStringLiteral("ScrollFill%1").arg(i));
  }
  ProcessEvents();
  QTRY_VERIFY_WITH_TIMEOUT(Cards().size() >= kFillCount + 2, 2000);

  auto* list = Find(QStringLiteral("editorVersionsList"));
  ASSERT_NE(list, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(
      list->property("contentHeight").toReal() > list->height() + 48.0, 2000);

  const qreal target_y = 96.0;
  ASSERT_TRUE(list->setProperty("contentY", target_y));
  ProcessEvents();
  QTRY_VERIFY_WITH_TIMEOUT(list->property("contentY").toReal() > 40.0, 1000);
  const qreal y_after_scroll = list->property("contentY").toReal();

  // Create via the production inline draft — grows the model (full reset path).
  const int create_before = backend_.create_count();
  Click(window_, Find(QStringLiteral("editorCreateVersionButton")));
  auto* field = Find(QStringLiteral("editorVersionNameField"));
  ASSERT_NE(field, nullptr);
  TypeText(window_, QStringLiteral("scrollednew"));
  Click(window_, Find(QStringLiteral("editorVersionAcceptButton")));
  ProcessEvents();
  QTRY_VERIFY_WITH_TIMEOUT(backend_.create_count() == create_before + 1, 2000);
  list = Find(QStringLiteral("editorVersionsList"));
  ASSERT_NE(list, nullptr);
  // Allow the panel's Qt.callLater restore to run.
  QTRY_VERIFY_WITH_TIMEOUT(
      qAbs(list->property("contentY").toReal() - y_after_scroll) <= 2.0, 2000);

  // Rename active Version — same-count path may use dataChanged; still preserve Y.
  const qreal y_before_rename = list->property("contentY").toReal();
  QQuickItem* active_card = nullptr;
  for (auto* card : Cards()) {
    if (card->property("versionActive").toBool()) active_card = card;
  }
  ASSERT_NE(active_card, nullptr);
  Click(window_, active_card->findChild<QQuickItem*>(QStringLiteral("editorRenameVersionButton")));
  field = Find(QStringLiteral("editorVersionNameField"));
  ASSERT_NE(field, nullptr);
  TypeText(window_, QStringLiteral("scrolledrenamed"));
  QTest::keyClick(window_, Qt::Key_Return);
  ProcessEvents();
  QTRY_VERIFY_WITH_TIMEOUT(backend_.rename_count() == 1, 2000);
  list = Find(QStringLiteral("editorVersionsList"));
  ASSERT_NE(list, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(
      qAbs(list->property("contentY").toReal() - y_before_rename) <= 2.0, 2000);

  // Checkout a non-active card — data-only active flag update.
  const qreal y_before_checkout = list->property("contentY").toReal();
  QQuickItem* inactive_card = nullptr;
  for (auto* card : Cards()) {
    if (!card->property("versionActive").toBool()) {
      inactive_card = card;
      break;
    }
  }
  ASSERT_NE(inactive_card, nullptr);
  const int checkout_before = backend_.checkout_count();
  Click(window_, inactive_card, QPointF(12.0, inactive_card->height() / 2.0));
  ProcessEvents();
  QTRY_VERIFY_WITH_TIMEOUT(backend_.checkout_count() == checkout_before + 1, 2000);
  list = Find(QStringLiteral("editorVersionsList"));
  ASSERT_NE(list, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(
      qAbs(list->property("contentY").toReal() - y_before_checkout) <= 2.0, 2000);
}

TEST_F(EditorVersionsPanelQmlTest,
       InlineDraftFocusLossCommitsChangedTextAndCancelsUnchanged) {
  ASSERT_NE(window_, nullptr) << warnings_.join('\n').toStdString();
  OpenVersionsPage();
  auto* page = Find(QStringLiteral("editorVersionsPageBody"));
  ASSERT_NE(page, nullptr);

  // Focus-loss with unchanged generated name: cancel, no create.
  Click(window_, Find(QStringLiteral("editorCreateVersionButton")));
  auto* field = Find(QStringLiteral("editorVersionNameField"));
  ASSERT_NE(field, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(field->property("visible").toBool(), 1000);
  const QString generated = field->property("text").toString().trimmed();
  ASSERT_FALSE(generated.isEmpty());
  // Move focus to Accept without activating it — triggers TextField.editingFinished
  // (deferred commitDraft(requireChanged=true) on the production panel).
  auto* accept = Find(QStringLiteral("editorVersionAcceptButton"));
  ASSERT_NE(accept, nullptr);
  accept->forceActiveFocus();
  ProcessEvents();
  QTRY_VERIFY_WITH_TIMEOUT(!page->property("draftVisible").toBool(), 2000);
  EXPECT_EQ(backend_.create_count(), 0);

  // Focus-loss with non-empty changed text: commit via the real model path.
  Click(window_, Find(QStringLiteral("editorCreateVersionButton")));
  field = Find(QStringLiteral("editorVersionNameField"));
  ASSERT_NE(field, nullptr);
  QTRY_VERIFY_WITH_TIMEOUT(field->property("visible").toBool(), 1000);
  TypeText(window_, QStringLiteral("focuscommit"));
  EXPECT_NE(field->property("text").toString().trimmed(), generated);
  accept = Find(QStringLiteral("editorVersionAcceptButton"));
  ASSERT_NE(accept, nullptr);
  accept->forceActiveFocus();
  ProcessEvents();
  QTRY_VERIFY_WITH_TIMEOUT(backend_.create_count() == 1, 2000);
  EXPECT_EQ(controller_.last_history_result().value(QStringLiteral("action")).toString(),
            QStringLiteral("createRootVersion"));
  QTRY_VERIFY_WITH_TIMEOUT(!page->property("draftVisible").toBool(), 2000);

  const QString created_id = QString::fromStdString(backend_.last_created_id().ToString());
  QQuickItem* created = nullptr;
  for (auto* card : Cards()) {
    if (card->property("versionId").toString() == created_id) created = card;
  }
  ASSERT_NE(created, nullptr);
  EXPECT_EQ(created->property("displayName").toString(), QStringLiteral("focuscommit"));
}

}  // namespace
}  // namespace alcedo::ui::test
