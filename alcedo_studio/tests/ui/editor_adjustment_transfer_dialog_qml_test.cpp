//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_adjustment_transfer_dialog_qml_test.cpp
/// @brief Production-dialog QML tests covering the three-pane Copy surface,
///        Select All / Clear ownership, derived three-state checks, monochrome
///        action text, scroll preservation, keyboard traversal, and the
///        read-only Paste summary.

#include <gtest/gtest.h>

#include <QAbstractItemModel>
#include <QCoreApplication>
#include <QEventLoop>
#include <QFont>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegularExpression>
#include <QTest>
#include <QTimer>
#include <QTranslator>
#include <QVariantMap>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>

#include "edit/graph/grade_owned_mask_support.hpp"
#include "edit/graph/graph_ids.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "edit/graph/pipeline_graph_commands.hpp"
#include "edit/history/commit_graph.hpp"
#include "edit/history/mini_git_working_history.hpp"
#include "edit/mask/mask_id.hpp"
#include "support/document_transfer_test_support.hpp"
#include "ui/alcedo_main/album_backend/adjustment_transfer_dialog_model.hpp"
#include "ui/alcedo_main/album_backend/adjustment_transfer_list_models.hpp"
#include "ui/alcedo_main/app_theme.hpp"

namespace alcedo::ui::test {
namespace {

auto QmlDirectory() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

auto DialogComponentUrl() -> QUrl {
  return QUrl::fromLocalFile(QmlDirectory() + QStringLiteral("/AdjustmentTransferDialog.qml"));
}

/// Journal + graph + immutable root document — the same MiniGit fixture shape
/// the dialog-model tests use. The live document is never part of this fixture.
class DialogQmlHistoryFixture {
 public:
  DialogQmlHistoryFixture() {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_             = std::filesystem::path{"build"} / "tmp" / "adjustment_transfer_dialog_qml" /
           std::string{info->name()};
    std::filesystem::create_directories(dir_);
    journal_path_ = dir_ / "image.wal";
    std::error_code ec;
    std::filesystem::remove(journal_path_, ec);

    journal_       = std::make_shared<MiniGitJournal>(journal_path_);
    graph_         = std::make_shared<CommitGraph>(CommitGraph::CreateEmpty(41));
    history_       = std::make_unique<MiniGitWorkingHistory>(graph_, journal_);
    root_document_ = std::make_shared<PipelineDocument>(CreateDefaultPipelineDocument());
  }

  void ReplaceRootDocument(PipelineDocument document) {
    root_document_ = std::make_shared<PipelineDocument>(std::move(document));
  }

  std::filesystem::path                  dir_;
  std::filesystem::path                  journal_path_;
  std::shared_ptr<MiniGitJournal>        journal_;
  std::shared_ptr<CommitGraph>           graph_;
  std::unique_ptr<MiniGitWorkingHistory> history_;
  std::shared_ptr<PipelineDocument>      root_document_;
};

// Harness window creates the real AdjustmentTransferDialog against the mode,
// summary rows, and dialog model supplied through context properties, then
// opens it on the window overlay. A Dialog needs an Overlay (window) to parent
// to, so the dialog is created against this ApplicationWindow.
constexpr char kHarnessQml[] = R"(
import QtQuick
import QtQuick.Controls.Basic

ApplicationWindow {
    id: root
    objectName: "adjustmentTransferDialogHarness"
    width: 1180
    height: 760
    visible: true
    property var dialog: null
    property string createError: ""

    Component.onCompleted: {
        var comp = Qt.createComponent(dialogComponentUrl)
        if (comp.status === Component.Error) {
            root.createError = comp.errorString()
            return
        }
        root.dialog = comp.createObject(root, {
            "mode": dialogMode,
            "pasteStrategy": "paste",
            "sourceTitle": "Copied source",
            "adjustmentRows": dialogRows,
            "dialogModel": transferDialogModel,
            "blurSource": null,
            "cornerRadius": 0
        })
        if (root.dialog) {
            root.dialog.open()
        } else {
            root.createError = comp.errorString()
        }
    }
}
)";

void           ProcessEvents(int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

class AdjustmentTransferDialogQmlHarness {
 public:
  QQmlApplicationEngine engine;
  QQuickWindow*         window = nullptr;
  QStringList           warnings;

  AdjustmentTransferDialogQmlHarness(const QString& mode, QObject* dialog_model,
                                     const QVariantList& rows) {
    AppTheme::RegisterFonts();
    AppTheme::Instance().setReduceMotion(true);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QObject::connect(&engine, &QQmlEngine::warnings, [this](const QList<QQmlError>& emitted) {
      for (const auto& warning : emitted) {
        warnings.push_back(warning.toString());
      }
    });
    engine.addImportPath(QStringLiteral("qrc:/"));
    engine.addImportPath(QmlDirectory());
    engine.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
    engine.rootContext()->setContextProperty(QStringLiteral("dialogComponentUrl"),
                                             DialogComponentUrl());
    engine.rootContext()->setContextProperty(QStringLiteral("dialogMode"), mode);
    engine.rootContext()->setContextProperty(QStringLiteral("dialogRows"), rows);
    engine.rootContext()->setContextProperty(QStringLiteral("transferDialogModel"), dialog_model);
    engine.loadData(QByteArray{kHarnessQml},
                    QUrl(QStringLiteral("file:///AdjustmentTransferDialogHarness.qml")));
    if (!engine.rootObjects().empty()) {
      window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
      if (window) {
        window->show();
        window->requestActivate();
      }
    }
    ProcessEvents(120);
  }

  auto dialog() const -> QObject* {
    return window ? window->property("dialog").value<QObject*>() : nullptr;
  }

  auto createError() const -> QString {
    return window ? window->property("createError").toString() : QString{};
  }

  // Bound-mode delegates (required properties) are not QObject children of the
  // view; they only appear in the visual childItems() tree. Lookup therefore
  // walks both trees.
  static void CollectByName(QQuickItem* item, const QString& name, QList<QObject*>* out) {
    if (item == nullptr) {
      return;
    }
    if (item->objectName() == name && !out->contains(item)) {
      out->push_back(item);
    }
    const auto children = item->childItems();
    for (auto* child : children) {
      CollectByName(child, name, out);
    }
  }

  auto visualRoots() const -> QList<QQuickItem*> {
    QList<QQuickItem*> roots;
    if (auto* d = dialog()) {
      if (auto* content = qobject_cast<QQuickItem*>(d->property("contentItem").value<QObject*>())) {
        roots.push_back(content);
      }
      if (auto* background =
              qobject_cast<QQuickItem*>(d->property("background").value<QObject*>())) {
        roots.push_back(background);
      }
    }
    if (window && window->contentItem()) {
      roots.push_back(window->contentItem());
    }
    return roots;
  }

  auto find(const QString& name) const -> QObject* {
    auto* d = dialog();
    if (d) {
      if (auto* child = d->findChild<QObject*>(name)) return child;
    }
    if (window) {
      if (auto* child = window->findChild<QObject*>(name)) return child;
    }
    const auto found = findAll(name);
    return found.isEmpty() ? nullptr : found.first();
  }

  auto findAll(const QString& name) const -> QList<QObject*> {
    QList<QObject*> result;
    if (auto* d = dialog()) {
      result = d->findChildren<QObject*>(name);
    }
    if (result.isEmpty() && window) {
      result = window->findChildren<QObject*>(name);
    }
    for (auto* root : visualRoots()) {
      CollectByName(root, name, &result);
    }
    return result;
  }
};

auto OpenModel(DialogQmlHistoryFixture& fixture) -> std::unique_ptr<AdjustmentTransferDialogModel> {
  auto        model = std::make_unique<AdjustmentTransferDialogModel>();
  std::string error;
  EXPECT_TRUE(model->OpenSource(fixture.graph_, fixture.root_document_, &error)) << error;
  return model;
}

auto FindDelegateByRole(const AdjustmentTransferDialogQmlHarness& harness,
                        const QString& delegate_name, const char* role, const QString& value)
    -> QQuickItem* {
  for (auto* item : harness.findAll(delegate_name)) {
    if (item->property(role).toString() == value) {
      return qobject_cast<QQuickItem*>(item);
    }
  }
  return nullptr;
}

void ClickAt(QQuickWindow* window, QObject* object, const QPointF& local) {
  auto* item = qobject_cast<QQuickItem*>(object);
  ASSERT_NE(item, nullptr);
  QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, item->mapToScene(local).toPoint());
}

void ClickCenter(QQuickWindow* window, QObject* object) {
  auto* item = qobject_cast<QQuickItem*>(object);
  ASSERT_NE(item, nullptr);
  ClickAt(window, item, QPointF(item->width() / 2.0, item->height() / 2.0));
}

void ClickRightEdge(QQuickWindow* window, QObject* object) {
  auto* item = qobject_cast<QQuickItem*>(object);
  ASSERT_NE(item, nullptr);
  ClickAt(window, item, QPointF(item->width() - 12.0, item->height() / 2.0));
}

auto ActionTextColor(QObject* button) -> QColor {
  auto* content = button->property("contentItem").value<QObject*>();
  if (content == nullptr) {
    return {};
  }
  return content->property("color").value<QColor>();
}

// ============================================================================
// Paste mode (pre-existing coverage, kept)
// ============================================================================

TEST(AdjustmentTransferDialogQmlTest, LoadsInPasteModeWithPasteAcceptLabel) {
  AdjustmentTransferDialogQmlHarness harness(QStringLiteral("paste"), nullptr,
                                             QVariantList{QVariantMap{
                                                 {"key", "exposure"},
                                                 {"label", "Exposure"},
                                                 {"section", "Tone"},
                                                 {"value", "+1.0"},
                                                 {"checked", true},
                                             }});
  ASSERT_NE(harness.window, nullptr) << harness.warnings.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings.isEmpty()) << harness.warnings.join('\n').toStdString();

  auto* dialog = harness.dialog();
  ASSERT_NE(dialog, nullptr) << harness.warnings.join("\n").toStdString()
                             << "| createError=" << harness.createError().toStdString();
  EXPECT_EQ(dialog->property("mode").toString(), QStringLiteral("paste"));
  EXPECT_EQ(harness.find(QStringLiteral("adjustmentTransferStrategySwitcher")), nullptr);
  EXPECT_EQ(harness.find(QStringLiteral("adjustmentTransferMergeNotice")), nullptr);

  auto* accept = harness.find(QStringLiteral("adjustmentTransferAcceptButton"));
  ASSERT_NE(accept, nullptr);
  EXPECT_TRUE(accept->property("text").toString().contains(QStringLiteral("Paste")));
  EXPECT_FALSE(accept->property("text").toString().contains(QStringLiteral("Merge")));
}

TEST(AdjustmentTransferDialogQmlTest, TransferSurfaceHasNoPipelineMergeOperation) {
  AdjustmentTransferDialogQmlHarness harness(QStringLiteral("paste"), nullptr, QVariantList{});
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings.join("\n").toStdString()
                                       << "| createError=" << harness.createError().toStdString();
  auto* dialog = harness.dialog();
  dialog->setProperty("pasteStrategy", QStringLiteral("merge"));
  ProcessEvents(40);
  auto* accept = harness.find(QStringLiteral("adjustmentTransferAcceptButton"));
  ASSERT_NE(accept, nullptr);
  EXPECT_TRUE(accept->property("text").toString().contains(QStringLiteral("Paste")));
  EXPECT_FALSE(accept->property("text").toString().contains(QStringLiteral("Merge")));
}

// ============================================================================
// Copy mode: three-pane surface
// ============================================================================

TEST(AdjustmentTransferDialogQmlTest, CopyDialogShowsVersionNodeAndItemPanes) {
  DialogQmlHistoryFixture fixture;
  auto                    model = OpenModel(fixture);
  ASSERT_NE(model, nullptr);

  AdjustmentTransferDialogQmlHarness harness(QStringLiteral("copy"), model.get(), QVariantList{});
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings.join("\n").toStdString()
                                       << "| createError=" << harness.createError().toStdString();
  ASSERT_TRUE(harness.warnings.isEmpty()) << harness.warnings.join('\n').toStdString();

  auto* version_list = harness.find(QStringLiteral("adjustmentTransferVersionList"));
  auto* node_list    = harness.find(QStringLiteral("adjustmentTransferNodeList"));
  auto* item_list    = harness.find(QStringLiteral("adjustmentTransferItemList"));
  ASSERT_NE(version_list, nullptr);
  ASSERT_NE(node_list, nullptr);
  ASSERT_NE(item_list, nullptr);
  EXPECT_TRUE(version_list->property("visible").toBool());
  EXPECT_TRUE(node_list->property("visible").toBool());
  EXPECT_TRUE(item_list->property("visible").toBool());
  EXPECT_EQ(version_list->property("count").toInt(), model->versions()->rowCount());
  EXPECT_EQ(node_list->property("count").toInt(), model->nodes()->rowCount());
  EXPECT_EQ(item_list->property("count").toInt(), model->items()->rowCount());
  EXPECT_GT(model->items()->rowCount(), 0);

  EXPECT_NE(harness.find(QStringLiteral("transferNodeSelectAll")), nullptr);
  EXPECT_NE(harness.find(QStringLiteral("transferItemSelectAll")), nullptr);
  EXPECT_NE(harness.find(QStringLiteral("transferNodeClearButton")), nullptr);
  EXPECT_NE(harness.find(QStringLiteral("transferItemClearButton")), nullptr);
}

TEST(AdjustmentTransferDialogQmlTest, NodeRowFocusDoesNotToggleItsCheckbox) {
  DialogQmlHistoryFixture fixture;
  auto                    model = OpenModel(fixture);
  ASSERT_NE(model, nullptr);

  AdjustmentTransferDialogQmlHarness harness(QStringLiteral("copy"), model.get(), QVariantList{});
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings.join("\n").toStdString()
                                       << "| createError=" << harness.createError().toStdString();

  const NodeId drt{"drt"};
  auto*        drt_row =
      FindDelegateByRole(harness, QStringLiteral("transferNodeDelegate"), "nodeId", "drt");
  ASSERT_NE(drt_row, nullptr);

  // Row-body click focuses the node without changing its derived check state.
  ClickRightEdge(harness.window, drt_row);
  ProcessEvents(40);
  EXPECT_EQ(model->focused_node_id(), QStringLiteral("drt"));
  EXPECT_EQ(model->NodeCheckStateForTesting(drt), Qt::Checked);
  EXPECT_EQ(drt_row->property("checkState").toInt(), static_cast<int>(Qt::Checked));

  // The row checkbox still toggles inclusion independently of focus.
  auto* drt_check = drt_row->findChild<QObject*>(QStringLiteral("transferNodeRowCheck"));
  ASSERT_NE(drt_check, nullptr);
  ClickCenter(harness.window, drt_check);
  ProcessEvents(40);
  EXPECT_EQ(model->NodeCheckStateForTesting(drt), Qt::Unchecked);
  EXPECT_EQ(model->focused_node_id(), QStringLiteral("drt"));
}

TEST(AdjustmentTransferDialogQmlTest, NodeAndItemSelectAllUseThreeStateCheckboxes) {
  DialogQmlHistoryFixture fixture;
  auto                    model = OpenModel(fixture);
  ASSERT_NE(model, nullptr);

  AdjustmentTransferDialogQmlHarness harness(QStringLiteral("copy"), model.get(), QVariantList{});
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings.join("\n").toStdString()
                                       << "| createError=" << harness.createError().toStdString();

  auto* node_select_all = harness.find(QStringLiteral("transferNodeSelectAll"));
  auto* item_select_all = harness.find(QStringLiteral("transferItemSelectAll"));
  ASSERT_NE(node_select_all, nullptr);
  ASSERT_NE(item_select_all, nullptr);

  // Partial selection marks both bulk checkboxes partially checked.
  model->SetItemChecked(QStringLiteral("grade.primary"),
                        QStringLiteral("adj:grade.primary.exposure"), false);
  ProcessEvents(60);
  EXPECT_TRUE(node_select_all->property("partiallyChecked").toBool());
  EXPECT_TRUE(item_select_all->property("partiallyChecked").toBool());
  EXPECT_EQ(model->all_nodes_check_state(), Qt::PartiallyChecked);
  EXPECT_EQ(model->focused_items_check_state(), Qt::PartiallyChecked);

  // Item Select All completes only the focused node.
  ClickCenter(harness.window, item_select_all);
  ProcessEvents(60);
  EXPECT_EQ(model->focused_items_check_state(), Qt::Checked);
  EXPECT_EQ(model->all_nodes_check_state(), Qt::Checked);

  // Clearing one node makes the global bulk checkbox partial again; its own
  // click selects every node.
  model->SetNodeChecked(QStringLiteral("drt"), false);
  ProcessEvents(60);
  EXPECT_TRUE(node_select_all->property("partiallyChecked").toBool());
  ClickCenter(harness.window, node_select_all);
  ProcessEvents(60);
  EXPECT_EQ(model->all_nodes_check_state(), Qt::Checked);
  EXPECT_TRUE(node_select_all->property("checked").toBool());
}

TEST(AdjustmentTransferDialogQmlTest, ClearButtonsUseWhiteTextAndClearTheirOwnedScope) {
  DialogQmlHistoryFixture fixture;
  auto                    model = OpenModel(fixture);
  ASSERT_NE(model, nullptr);

  AdjustmentTransferDialogQmlHarness harness(QStringLiteral("copy"), model.get(), QVariantList{});
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings.join("\n").toStdString()
                                       << "| createError=" << harness.createError().toStdString();

  auto* node_clear = harness.find(QStringLiteral("transferNodeClearButton"));
  auto* item_clear = harness.find(QStringLiteral("transferItemClearButton"));
  ASSERT_NE(node_clear, nullptr);
  ASSERT_NE(item_clear, nullptr);
  EXPECT_EQ(ActionTextColor(node_clear), QColor(QStringLiteral("#FFFFFF")));
  EXPECT_EQ(ActionTextColor(item_clear), QColor(QStringLiteral("#FFFFFF")));

  // Item Clear only clears the focused node; other nodes stay checked.
  ClickCenter(harness.window, item_clear);
  ProcessEvents(60);
  EXPECT_EQ(model->NodeCheckStateForTesting(NodeId{"grade.primary"}), Qt::Unchecked);
  EXPECT_EQ(model->NodeCheckStateForTesting(NodeId{"drt"}), Qt::Checked);
  EXPECT_EQ(model->all_nodes_check_state(), Qt::PartiallyChecked);
  EXPECT_TRUE(model->can_copy());

  // Node Clear clears every node and disables Copy.
  ClickCenter(harness.window, node_clear);
  ProcessEvents(60);
  EXPECT_EQ(model->all_nodes_check_state(), Qt::Unchecked);
  EXPECT_FALSE(model->can_copy());
  auto* accept = harness.find(QStringLiteral("adjustmentTransferAcceptButton"));
  ASSERT_NE(accept, nullptr);
  EXPECT_FALSE(accept->property("enabled").toBool());
}

TEST(AdjustmentTransferDialogQmlTest, TransferDialogActionButtonsAlwaysUseWhiteEnabledText) {
  DialogQmlHistoryFixture fixture;
  auto                    model = OpenModel(fixture);
  ASSERT_NE(model, nullptr);

  AdjustmentTransferDialogQmlHarness harness(QStringLiteral("copy"), model.get(), QVariantList{});
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings.join("\n").toStdString()
                                       << "| createError=" << harness.createError().toStdString();

  const QColor white{QStringLiteral("#FFFFFF")};
  for (const char* name : {"adjustmentTransferCancelButton", "adjustmentTransferAcceptButton",
                           "transferNodeClearButton", "transferItemClearButton"}) {
    auto* button = harness.find(QString::fromLatin1(name));
    ASSERT_NE(button, nullptr) << name;
    ASSERT_TRUE(button->property("enabled").toBool()) << name;
    EXPECT_EQ(ActionTextColor(button), white) << name;
  }
}

TEST(AdjustmentTransferDialogQmlTest, TransferDialogHasNoNumericFooterSummary) {
  DialogQmlHistoryFixture fixture;
  auto                    model = OpenModel(fixture);
  ASSERT_NE(model, nullptr);

  AdjustmentTransferDialogQmlHarness harness(QStringLiteral("copy"), model.get(), QVariantList{});
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings.join("\n").toStdString()
                                       << "| createError=" << harness.createError().toStdString();

  auto* footer = harness.find(QStringLiteral("adjustmentTransferFooter"));
  ASSERT_NE(footer, nullptr);
  const QRegularExpression digit{QStringLiteral("\\d")};
  for (auto* child : footer->findChildren<QObject*>()) {
    const QString text = child->property("text").toString();
    EXPECT_FALSE(text.contains(digit)) << "numeric footer text: " << text.toStdString();
  }
}

TEST(AdjustmentTransferDialogQmlTest, MasksAppearAsOneAllOrNoneCheckbox) {
  DialogQmlHistoryFixture fixture;
  auto                    document = CreateDefaultPipelineDocument();
  grade_mask_test::AddRadialMask(document, MaskId{"mask.a"});
  grade_mask_test::AddLinearGradientMask(document, MaskId{"mask.b"});
  fixture.ReplaceRootDocument(std::move(document));
  auto model = OpenModel(fixture);
  ASSERT_NE(model, nullptr);

  AdjustmentTransferDialogQmlHarness harness(QStringLiteral("copy"), model.get(), QVariantList{});
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings.join("\n").toStdString()
                                       << "| createError=" << harness.createError().toStdString();

  // The Masks row is last in the focused node's list; scroll the delegate into
  // existence before counting it.
  auto* item_list = harness.find(QStringLiteral("adjustmentTransferItemList"));
  ASSERT_NE(item_list, nullptr);
  item_list->setProperty("contentY", item_list->property("contentHeight").toDouble() -
                                         item_list->property("height").toDouble());
  ProcessEvents(60);

  int         masks_rows = 0;
  QQuickItem* masks_row  = nullptr;
  for (auto* item : harness.findAll(QStringLiteral("transferItemDelegate"))) {
    if (item->property("itemKey").toString() == QStringLiteral("masks")) {
      ++masks_rows;
      masks_row = qobject_cast<QQuickItem*>(item);
    }
  }
  ASSERT_EQ(masks_rows, 1) << "the Mask set must stay one all-or-none row";
  ASSERT_NE(masks_row, nullptr);
  EXPECT_TRUE(masks_row->property("enabled").toBool());
  EXPECT_EQ(masks_row->property("displayValue").toString(), QStringLiteral("2"));

  auto* checkbox = masks_row->findChild<QObject*>(QStringLiteral("transferItemCheckRow"));
  ASSERT_NE(checkbox, nullptr);
  EXPECT_EQ(checkbox->property("accessibleText").toString(),
            QStringLiteral("Transfer all masks in this node"));
}

TEST(AdjustmentTransferDialogQmlTest, CheckboxChangesPreserveAllListScrollPositions) {
  DialogQmlHistoryFixture fixture;
  auto                    document = CreateDefaultPipelineDocument();
  for (int index = 0; index < 12; ++index) {
    const auto node_id = NodeId{"grade.extra." + std::to_string(index)};
    ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, node_id).empty());
  }
  fixture.ReplaceRootDocument(std::move(document));
  for (int index = 0; index < 12; ++index) {
    const auto created =
        fixture.graph_->GetActiveVersionRef().created_at + static_cast<std::time_t>(index + 1);
    fixture.graph_->CreateVersionRefAtActiveHead("Version " + std::to_string(index), created);
  }
  auto model = OpenModel(fixture);
  ASSERT_NE(model, nullptr);

  AdjustmentTransferDialogQmlHarness harness(QStringLiteral("copy"), model.get(), QVariantList{});
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings.join("\n").toStdString()
                                       << "| createError=" << harness.createError().toStdString();

  auto* version_list = harness.find(QStringLiteral("adjustmentTransferVersionList"));
  auto* node_list    = harness.find(QStringLiteral("adjustmentTransferNodeList"));
  auto* item_list    = harness.find(QStringLiteral("adjustmentTransferItemList"));
  ASSERT_NE(version_list, nullptr);
  ASSERT_NE(node_list, nullptr);
  ASSERT_NE(item_list, nullptr);

  for (auto* list : {version_list, node_list, item_list}) {
    list->setProperty("contentY", list->property("contentHeight").toDouble() -
                                      list->property("height").toDouble());
  }
  ProcessEvents(60);
  const double version_y = version_list->property("contentY").toDouble();
  const double node_y    = node_list->property("contentY").toDouble();
  const double item_y    = item_list->property("contentY").toDouble();
  EXPECT_GT(version_y, 0.0);
  EXPECT_GT(node_y, 0.0);
  EXPECT_GT(item_y, 0.0);

  model->SetItemChecked(QStringLiteral("grade.primary"),
                        QStringLiteral("adj:grade.primary.exposure"), false);
  model->SetNodeChecked(QStringLiteral("drt"), false);
  ProcessEvents(80);
  EXPECT_DOUBLE_EQ(version_list->property("contentY").toDouble(), version_y);
  EXPECT_DOUBLE_EQ(node_list->property("contentY").toDouble(), node_y);
  EXPECT_DOUBLE_EQ(item_list->property("contentY").toDouble(), item_y);

  // Focus swaps the item column contents; the Version and node columns keep
  // their scroll offsets because only derived check/focus roles change.
  model->FocusNode(QStringLiteral("grade.extra.3"));
  ProcessEvents(80);
  EXPECT_DOUBLE_EQ(version_list->property("contentY").toDouble(), version_y);
  EXPECT_DOUBLE_EQ(node_list->property("contentY").toDouble(), node_y);
}

// ============================================================================
// Paste mode: read-only node and item summary
// ============================================================================

TEST(AdjustmentTransferDialogQmlTest, PasteDialogShowsReadOnlyNodeAndItemSummary) {
  DialogQmlHistoryFixture fixture;
  auto                    model = OpenModel(fixture);
  ASSERT_NE(model, nullptr);
  const QVariantList rows = model->SelectionSummary();
  ASSERT_GT(rows.size(), 0);

  AdjustmentTransferDialogQmlHarness harness(QStringLiteral("paste"), nullptr, rows);
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings.join("\n").toStdString()
                                       << "| createError=" << harness.createError().toStdString();
  ASSERT_TRUE(harness.warnings.isEmpty()) << harness.warnings.join('\n').toStdString();

  auto* node_list = harness.find(QStringLiteral("adjustmentTransferNodeList"));
  auto* item_list = harness.find(QStringLiteral("adjustmentTransferItemList"));
  ASSERT_NE(node_list, nullptr);
  ASSERT_NE(item_list, nullptr);
  EXPECT_EQ(node_list->property("count").toInt(), 2);

  auto* node_controls = harness.find(QStringLiteral("transferNodeHeaderControls"));
  auto* item_controls = harness.find(QStringLiteral("transferItemHeaderControls"));
  ASSERT_NE(node_controls, nullptr);
  ASSERT_NE(item_controls, nullptr);
  EXPECT_FALSE(node_controls->property("visible").toBool());
  EXPECT_FALSE(item_controls->property("visible").toBool());
  EXPECT_EQ(harness.findAll(QStringLiteral("transferItemCheckRow")).size(), 0);
  EXPECT_GT(harness.findAll(QStringLiteral("transferPasteItemRow")).size(), 0);
  for (auto* check : harness.findAll(QStringLiteral("transferNodeRowCheck"))) {
    EXPECT_FALSE(check->property("visible").toBool())
        << "paste node rows must not show selection checkboxes";
  }

  // Paste rows never carry a checkbox; item delegates are read-only rows.
  int first_node_items = 0;
  int other_node_items = 0;
  for (const auto& entry : rows) {
    if (entry.toMap().value(QStringLiteral("node")).toInt() == 0) {
      ++first_node_items;
    } else {
      ++other_node_items;
    }
  }
  ASSERT_GT(first_node_items, 0);
  ASSERT_GT(other_node_items, 0);
  EXPECT_EQ(item_list->property("count").toInt(), first_node_items);

  // Node focus still swaps the read-only item column — no selection mutation
  // exists in paste mode.
  auto* drt_row =
      FindDelegateByRole(harness, QStringLiteral("transferNodeDelegate"), "nodeId", "n1");
  ASSERT_NE(drt_row, nullptr);
  ClickRightEdge(harness.window, drt_row);
  ProcessEvents(60);
  EXPECT_EQ(item_list->property("count").toInt(), other_node_items);
}

// ============================================================================
// Theme and token usage
// ============================================================================

TEST(AdjustmentTransferDialogQmlTest, TransferDialogUsesThemeTypographyAndSelectionTokens) {
  DialogQmlHistoryFixture fixture;
  auto                    model = OpenModel(fixture);
  ASSERT_NE(model, nullptr);

  AdjustmentTransferDialogQmlHarness harness(QStringLiteral("copy"), model.get(), QVariantList{});
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings.join("\n").toStdString()
                                       << "| createError=" << harness.createError().toStdString();

  const auto& theme = AppTheme::Instance();

  // Focused node row uses the monochrome selected well + ink.
  auto* focused_row = FindDelegateByRole(harness, QStringLiteral("transferNodeDelegate"), "nodeId",
                                         "grade.primary");
  ASSERT_NE(focused_row, nullptr);
  ASSERT_TRUE(focused_row->property("focused").toBool());
  const auto children = focused_row->childItems();
  ASSERT_FALSE(children.isEmpty());
  auto* fill = children.first();
  EXPECT_EQ(fill->property("color").value<QColor>(), theme.editorListSelectedFillColor());

  auto* layout = children.size() > 2 ? children.at(2) : nullptr;
  ASSERT_NE(layout, nullptr);
  const auto layout_children = layout->childItems();
  ASSERT_GE(layout_children.size(), 2);
  auto* name_label = layout_children.at(1);
  EXPECT_EQ(name_label->property("color").value<QColor>(), theme.editorListSelectedInkColor());
  EXPECT_EQ(name_label->property("font").value<QFont>().family(), theme.uiFontFamily());

  // List wells sit on the sunken base surface.
  auto* node_list =
      qobject_cast<QQuickItem*>(harness.find(QStringLiteral("adjustmentTransferNodeList")));
  ASSERT_NE(node_list, nullptr);
  ASSERT_NE(node_list->parentItem(), nullptr);
  EXPECT_EQ(node_list->parentItem()->property("color").value<QColor>(), theme.bgBaseColor());

  // The primary action keeps the documented accent CTA with white text.
  auto* accept = harness.find(QStringLiteral("adjustmentTransferAcceptButton"));
  ASSERT_NE(accept, nullptr);
  EXPECT_EQ(accept->property("kind").toString(), QStringLiteral("accent"));
  EXPECT_EQ(ActionTextColor(accept), QColor(QStringLiteral("#FFFFFF")));
}

// ============================================================================
// Keyboard traversal
// ============================================================================

TEST(AdjustmentTransferDialogQmlTest, TransferDialogKeyboardOrderReachesAllThreePanesAndActions) {
  DialogQmlHistoryFixture fixture;
  auto                    document = CreateDefaultPipelineDocument();
  ASSERT_TRUE(AddCleanColorGrade(document, NodeId{"drt"}, NodeId{"grade.extra"}).empty());
  fixture.ReplaceRootDocument(std::move(document));
  auto model = OpenModel(fixture);
  ASSERT_NE(model, nullptr);

  AdjustmentTransferDialogQmlHarness harness(QStringLiteral("copy"), model.get(), QVariantList{});
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings.join("\n").toStdString()
                                       << "| createError=" << harness.createError().toStdString();
  auto* window = harness.window;
  ASSERT_NE(window, nullptr);

  // keyNavigationEnabled delegates active focus to the current row; check the
  // focused item's visual ancestry instead of the view identity.
  const auto focus_within = [&window](QObject* root) {
    for (auto* item = window->activeFocusItem(); item != nullptr; item = item->parentItem()) {
      if (item == root) {
        return true;
      }
    }
    return false;
  };

  auto* version_list = harness.find(QStringLiteral("adjustmentTransferVersionList"));
  auto* node_list    = harness.find(QStringLiteral("adjustmentTransferNodeList"));
  auto* item_list    = harness.find(QStringLiteral("adjustmentTransferItemList"));
  ASSERT_NE(version_list, nullptr);
  ASSERT_NE(node_list, nullptr);
  ASSERT_NE(item_list, nullptr);

  ASSERT_TRUE(QMetaObject::invokeMethod(version_list, "forceActiveFocus"));
  ProcessEvents(40);
  ASSERT_TRUE(focus_within(version_list))
      << "active="
      << (window->activeFocusItem()
              ? (QString::fromLatin1(window->activeFocusItem()->metaObject()->className()) +
                 QLatin1Char(':') + window->activeFocusItem()->objectName())
                    .toStdString()
              : std::string{"null"});

  QStringList focus_chain;
  for (int step = 0; step < 14; ++step) {
    QTest::keyClick(window, Qt::Key_Tab);
    ProcessEvents(20);
    // Delegates can own active focus; record the full named ancestor chain so
    // list stops are identifiable by their view objectName.
    QStringList names;
    for (auto* item = window->activeFocusItem(); item != nullptr; item = item->parentItem()) {
      if (!item->objectName().isEmpty()) {
        names.push_back(item->objectName());
      }
    }
    focus_chain.push_back(names.join(QLatin1Char('/')));
  }
  const auto index_of = [&focus_chain](const char* name) {
    const QString wanted = QString::fromLatin1(name);
    for (int row = 0; row < focus_chain.size(); ++row) {
      if (focus_chain.at(row).split(QLatin1Char('/')).contains(wanted)) {
        return row;
      }
    }
    return -1;
  };
  ASSERT_GE(index_of("adjustmentTransferNodeList"), 0) << focus_chain.join(' ').toStdString();
  ASSERT_GE(index_of("adjustmentTransferItemList"), 0) << focus_chain.join(' ').toStdString();
  ASSERT_GE(index_of("adjustmentTransferCancelButton"), 0) << focus_chain.join(' ').toStdString();
  ASSERT_GE(index_of("adjustmentTransferAcceptButton"), 0) << focus_chain.join(' ').toStdString();
  EXPECT_LT(index_of("adjustmentTransferNodeList"), index_of("adjustmentTransferItemList"));
  EXPECT_LT(index_of("adjustmentTransferItemList"), index_of("adjustmentTransferCancelButton"));
  EXPECT_LT(index_of("adjustmentTransferCancelButton"), index_of("adjustmentTransferAcceptButton"));

  // Arrow keys move inside the node list; Enter focuses the row without
  // touching its checkbox; Space toggles it.
  ASSERT_TRUE(QMetaObject::invokeMethod(node_list, "forceActiveFocus"));
  ProcessEvents(40);
  ASSERT_TRUE(focus_within(node_list));
  node_list->setProperty("currentIndex", 1);
  QTest::keyClick(window, Qt::Key_Return);
  ProcessEvents(60);
  EXPECT_EQ(model->focused_node_id(), QStringLiteral("grade.extra"));
  EXPECT_EQ(model->NodeCheckStateForTesting(NodeId{"grade.extra"}), Qt::Checked);
  QTest::keyClick(window, Qt::Key_Space);
  ProcessEvents(60);
  EXPECT_EQ(model->NodeCheckStateForTesting(NodeId{"grade.extra"}), Qt::Unchecked);

  ASSERT_TRUE(QMetaObject::invokeMethod(item_list, "forceActiveFocus"));
  ProcessEvents(40);
  ASSERT_TRUE(focus_within(item_list));
  item_list->setProperty("currentIndex", 0);
  QTest::keyClick(window, Qt::Key_Space);
  ProcessEvents(60);
  EXPECT_EQ(model->focused_items_check_state(), Qt::PartiallyChecked);

  QTest::keyClick(window, Qt::Key_Escape);
  ProcessEvents(60);
  EXPECT_FALSE(harness.dialog()->property("opened").toBool());
}

// ============================================================================
// zh_CN catalog: compiled QM drives the real production dialog in both modes.
// ============================================================================

auto AccessibleNameOf(QObject* object) -> QString {
  if (object == nullptr) {
    return {};
  }
  QQmlProperty property(object, QStringLiteral("Accessible.name"), qmlContext(object));
  const auto   name = property.read().toString();
  if (!name.isEmpty()) {
    return name;
  }
  return object->property("Accessible.name").toString();
}

TEST(AdjustmentTransferDialogQmlTest,
     SimplifiedChineseCatalogRetranslatesAdjustmentTransferPanesInPlace) {
  QTranslator zh;
  ASSERT_TRUE(zh.load(QStringLiteral(ALCEDO_ZH_CN_QM_FILE)))
      << "compiled zh_CN catalog missing at " << ALCEDO_ZH_CN_QM_FILE;
  ASSERT_TRUE(QCoreApplication::installTranslator(&zh));

  DialogQmlHistoryFixture fixture;
  auto                    document = CreateDefaultPipelineDocument();
  grade_mask_test::AddRadialMask(document, MaskId{"mask.a"});
  fixture.ReplaceRootDocument(std::move(document));
  auto model = OpenModel(fixture);
  ASSERT_NE(model, nullptr);

  AdjustmentTransferDialogQmlHarness harness(QStringLiteral("copy"), model.get(), QVariantList{});
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings.join("\n").toStdString()
                                       << "| createError=" << harness.createError().toStdString();
  ASSERT_TRUE(harness.warnings.isEmpty()) << harness.warnings.join('\n').toStdString();

  auto* dialog       = harness.dialog();

  // Pane titles, bulk actions, and footer resolve through the compiled catalog.
  auto* version_pane = harness.find(QStringLiteral("adjustmentTransferVersionPane"));
  auto* node_pane    = harness.find(QStringLiteral("adjustmentTransferNodePane"));
  ASSERT_NE(version_pane, nullptr);
  ASSERT_NE(node_pane, nullptr);
  EXPECT_EQ(version_pane->property("title").toString(), QStringLiteral("源版本"));
  EXPECT_EQ(node_pane->property("title").toString(), QStringLiteral("节点"));

  auto* node_select_all = harness.find(QStringLiteral("transferNodeSelectAll"));
  auto* item_select_all = harness.find(QStringLiteral("transferItemSelectAll"));
  auto* node_clear      = harness.find(QStringLiteral("transferNodeClearButton"));
  auto* item_clear      = harness.find(QStringLiteral("transferItemClearButton"));
  auto* cancel          = harness.find(QStringLiteral("adjustmentTransferCancelButton"));
  auto* accept          = harness.find(QStringLiteral("adjustmentTransferAcceptButton"));
  auto* close           = harness.find(QStringLiteral("adjustmentTransferCloseButton"));
  ASSERT_NE(node_select_all, nullptr);
  ASSERT_NE(item_select_all, nullptr);
  ASSERT_NE(node_clear, nullptr);
  ASSERT_NE(item_clear, nullptr);
  ASSERT_NE(cancel, nullptr);
  ASSERT_NE(accept, nullptr);
  ASSERT_NE(close, nullptr);
  EXPECT_EQ(node_select_all->property("text").toString(), QStringLiteral("全选"));
  EXPECT_EQ(item_select_all->property("text").toString(), QStringLiteral("全选"));
  EXPECT_EQ(node_clear->property("text").toString(), QStringLiteral("清除"));
  EXPECT_EQ(item_clear->property("text").toString(), QStringLiteral("清除"));
  EXPECT_EQ(cancel->property("text").toString(), QStringLiteral("取消"));
  EXPECT_EQ(accept->property("text").toString(), QStringLiteral("复制调整"));
  EXPECT_EQ(AccessibleNameOf(close), QStringLiteral("关闭"));

  // Section vocabulary mirrors the Adjustment Stack terms.
  auto* item_pane = harness.find(QStringLiteral("adjustmentTransferItemPane"));
  ASSERT_NE(item_pane, nullptr);
  // QML functions surface as QVariant methods; the return arg must be a
  // QVariant, not QString, for invokeMethod to match the signature.
  const auto section_name = [item_pane](int value) {
    QVariant name;
    EXPECT_TRUE(QMetaObject::invokeMethod(item_pane, "sectionName", Q_RETURN_ARG(QVariant, name),
                                          Q_ARG(QVariant, value)));
    return name.toString();
  };
  EXPECT_EQ(section_name(0), QStringLiteral("节点"));
  EXPECT_EQ(section_name(1), QStringLiteral("色调"));
  EXPECT_EQ(section_name(2), QStringLiteral("外观"));
  EXPECT_EQ(section_name(3), QStringLiteral("LUT"));
  EXPECT_EQ(section_name(4), QStringLiteral("显示变换"));
  EXPECT_EQ(section_name(5), QStringLiteral("蒙版"));
  EXPECT_EQ(section_name(99), QStringLiteral("其他"));

  // Node-row and mask-row accessibility text translate; the %1 argument is
  // backend data and stays untouched.
  auto* drt_row =
      FindDelegateByRole(harness, QStringLiteral("transferNodeDelegate"), "nodeId", "drt");
  ASSERT_NE(drt_row, nullptr);
  auto* drt_check = drt_row->findChild<QObject*>(QStringLiteral("transferNodeRowCheck"));
  ASSERT_NE(drt_check, nullptr);
  const auto node_check_text = drt_check->property("accessibleText").toString();
  EXPECT_TRUE(node_check_text.contains(QStringLiteral("选择"))) << node_check_text.toStdString();
  EXPECT_TRUE(node_check_text.contains(QStringLiteral("中的所有项目")))
      << node_check_text.toStdString();

  auto* item_list = harness.find(QStringLiteral("adjustmentTransferItemList"));
  ASSERT_NE(item_list, nullptr);
  item_list->setProperty("contentY", item_list->property("contentHeight").toDouble() -
                                         item_list->property("height").toDouble());
  ProcessEvents(60);
  QQuickItem* masks_row = nullptr;
  for (auto* item : harness.findAll(QStringLiteral("transferItemDelegate"))) {
    if (item->property("itemKey").toString() == QStringLiteral("masks")) {
      masks_row = qobject_cast<QQuickItem*>(item);
    }
  }
  ASSERT_NE(masks_row, nullptr);
  auto* masks_check = masks_row->findChild<QObject*>(QStringLiteral("transferItemCheckRow"));
  ASSERT_NE(masks_check, nullptr);
  EXPECT_EQ(masks_check->property("accessibleText").toString(),
            QStringLiteral("转移此节点中的所有蒙版"));

  // Retranslation flips the same loaded dialog back to English in place.
  QCoreApplication::removeTranslator(&zh);
  harness.engine.retranslate();
  ProcessEvents(60);
  EXPECT_EQ(harness.dialog(), dialog);
  EXPECT_EQ(accept->property("text").toString(), QStringLiteral("Copy Adjustments"));
  EXPECT_EQ(cancel->property("text").toString(), QStringLiteral("Cancel"));
  EXPECT_EQ(node_pane->property("title").toString(), QStringLiteral("Nodes"));

  ASSERT_TRUE(QCoreApplication::installTranslator(&zh));
  harness.engine.retranslate();
  ProcessEvents(60);
  EXPECT_EQ(harness.dialog(), dialog);
  EXPECT_EQ(accept->property("text").toString(), QStringLiteral("复制调整"));
  EXPECT_EQ(node_pane->property("title").toString(), QStringLiteral("节点"));

  QCoreApplication::removeTranslator(&zh);
}

TEST(AdjustmentTransferDialogQmlTest,
     SimplifiedChineseAdjustmentTransferLabelsRemainVisibleAtSupportedWidths) {
  QTranslator zh;
  ASSERT_TRUE(zh.load(QStringLiteral(ALCEDO_ZH_CN_QM_FILE)))
      << "compiled zh_CN catalog missing at " << ALCEDO_ZH_CN_QM_FILE;
  ASSERT_TRUE(QCoreApplication::installTranslator(&zh));

  // Empty paste package: read-only panes show the Chinese empty hint and the
  // generic item-pane title.
  {
    AdjustmentTransferDialogQmlHarness empty_harness(QStringLiteral("paste"), nullptr,
                                                     QVariantList{});
    ASSERT_NE(empty_harness.dialog(), nullptr) << empty_harness.warnings.join("\n").toStdString();
    auto* empty_hint = empty_harness.find(QStringLiteral("adjustmentTransferEmptyHint"));
    ASSERT_NE(empty_hint, nullptr);
    EXPECT_TRUE(empty_hint->property("visible").toBool());
    EXPECT_EQ(empty_hint->property("text").toString(), QStringLiteral("没有可转移的调整。"));
    auto* item_pane = empty_harness.find(QStringLiteral("adjustmentTransferItemPane"));
    ASSERT_NE(item_pane, nullptr);
    EXPECT_EQ(item_pane->property("title").toString(), QStringLiteral("要粘贴的参数"));
  }

  DialogQmlHistoryFixture fixture;
  auto                    model = OpenModel(fixture);
  ASSERT_NE(model, nullptr);
  const QVariantList rows = model->SelectionSummary();
  ASSERT_GT(rows.size(), 0);

  AdjustmentTransferDialogQmlHarness harness(QStringLiteral("paste"), nullptr, rows);
  ASSERT_NE(harness.dialog(), nullptr) << harness.warnings.join("\n").toStdString()
                                       << "| createError=" << harness.createError().toStdString();
  ASSERT_TRUE(harness.warnings.isEmpty()) << harness.warnings.join('\n').toStdString();

  auto* accept = harness.find(QStringLiteral("adjustmentTransferAcceptButton"));
  auto* cancel = harness.find(QStringLiteral("adjustmentTransferCancelButton"));
  auto* close  = harness.find(QStringLiteral("adjustmentTransferCloseButton"));
  ASSERT_NE(accept, nullptr);
  ASSERT_NE(cancel, nullptr);
  ASSERT_NE(close, nullptr);
  EXPECT_EQ(accept->property("text").toString(), QStringLiteral("粘贴调整"));
  EXPECT_EQ(cancel->property("text").toString(), QStringLiteral("取消"));
  EXPECT_EQ(AccessibleNameOf(close), QStringLiteral("关闭"));

  // Read-only panes keep the Chinese pane title and hide selection controls.
  auto* node_pane = harness.find(QStringLiteral("adjustmentTransferNodePane"));
  ASSERT_NE(node_pane, nullptr);
  EXPECT_EQ(node_pane->property("title").toString(), QStringLiteral("节点"));
  auto* node_controls = harness.find(QStringLiteral("transferNodeHeaderControls"));
  auto* item_controls = harness.find(QStringLiteral("transferItemHeaderControls"));
  ASSERT_NE(node_controls, nullptr);
  ASSERT_NE(item_controls, nullptr);
  EXPECT_FALSE(node_controls->property("visible").toBool());
  EXPECT_FALSE(item_controls->property("visible").toBool());

  // Paste item rows expose the translated section vocabulary.
  auto* item_pane = harness.find(QStringLiteral("adjustmentTransferItemPane"));
  ASSERT_NE(item_pane, nullptr);
  QVariant section;
  ASSERT_TRUE(QMetaObject::invokeMethod(item_pane, "sectionName", Q_RETURN_ARG(QVariant, section),
                                        Q_ARG(QVariant, 5)));
  EXPECT_EQ(section.toString(), QStringLiteral("蒙版"));

  // The dialog stays inside the harness window at a reduced supported width
  // and the Chinese pane titles remain visible.
  auto* dialog = harness.dialog();
  harness.window->resize(760, 600);
  ProcessEvents(80);
  EXPECT_LE(dialog->property("width").toReal(), harness.window->width());
  EXPECT_GE(dialog->property("x").toReal(), 0.0);
  EXPECT_TRUE(node_pane->property("visible").toBool());
  EXPECT_TRUE(item_pane->property("visible").toBool());
  EXPECT_EQ(node_pane->property("title").toString(), QStringLiteral("节点"));
  EXPECT_EQ(accept->property("text").toString(), QStringLiteral("粘贴调整"));

  QCoreApplication::removeTranslator(&zh);
}

}  // namespace
}  // namespace alcedo::ui::test
