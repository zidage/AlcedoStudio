//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_adjustment_transfer_actions_qml_test.cpp
/// @brief Verifies Paste/Merge policy checks at the extracted QML action boundary.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QEventLoop>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <filesystem>

#include "ui/alcedo_main/app_theme.hpp"

namespace alcedo::ui::test {
namespace {

class RecordingAdjustmentTransfer final : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool packageAvailable READ packageAvailable WRITE setPackageAvailable)
  Q_PROPERTY(QString packageSourceTitle READ packageSourceTitle CONSTANT)
  Q_PROPERTY(QVariantList packageSummary READ packageSummary CONSTANT)

 public:
  RecordingAdjustmentTransfer() {
    package_summary_.push_back(QVariantMap{{QStringLiteral("key"), QStringLiteral("exposure")},
                                           {QStringLiteral("label"), QStringLiteral("Exposure")},
                                           {QStringLiteral("section"), QStringLiteral("Tone")},
                                           {QStringLiteral("value"), QStringLiteral("+1.0")}});
  }

  auto packageAvailable() const -> bool { return package_available_; }
  void setPackageAvailable(bool available) { package_available_ = available; }
  auto packageSourceTitle() const -> QString { return QStringLiteral("Copied source"); }
  auto packageSummary() const -> QVariantList { return package_summary_; }

  Q_INVOKABLE QVariantMap Paste(const QVariantList& targets, const QString& strategy) {
    ++paste_call_count_;
    last_targets_  = targets;
    last_strategy_ = strategy;
    return {{QStringLiteral("success"), true},
            {QStringLiteral("message"), QStringLiteral("Applied")}};
  }

  auto paste_call_count() const -> int { return paste_call_count_; }
  auto last_targets() const -> QVariantList { return last_targets_; }
  auto last_strategy() const -> QString { return last_strategy_; }

 private:
  bool         package_available_ = true;
  QVariantList package_summary_;
  int          paste_call_count_ = 0;
  QVariantList last_targets_;
  QString      last_strategy_;
};

constexpr char kHarnessQml[] = R"(
import QtQuick
import QtQuick.Controls

ApplicationWindow {
    id: root
    objectName: "editorAdjustmentTransferActionsHarness"
    width: 320
    height: 80
    visible: true
    property string lastMessage: ""

    property QtObject dialog: QtObject {
        objectName: "adjustmentTransferDialog"
        property string mode: "copy"
        property string pasteStrategy: "merge"
        property string sourceTitle: ""
        property int targetCount: 0
        property var adjustmentRows: []
        property int openCount: 0

        function open() {
            openCount += 1
        }
    }

    property QtObject policy: QtObject {
        objectName: "interactionPolicy"
        property bool canPasteAdjustments: true
        property bool canMergeAdjustments: true
        property string pasteAdjustmentsReason: "Paste is blocked"
        property string mergeAdjustmentsReason: "Merge is blocked"
    }

    Loader {
        id: actionLoader
        objectName: "actionLoader"
        source: actionSourceUrl

        onLoaded: {
            if (!item) {
                return
            }
            item.adjustmentTransfer = adjustmentTransferFake
            item.adjustmentTransferDialog = dialog
            item.interactionPolicy = policy
            item.pendingTargets = [101, 102]
        }
    }

    Connections {
        target: actionLoader.item
        function onMessageRequested(message) {
            root.lastMessage = message
        }
    }
}
)";

auto           QmlDirectory() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

auto ActionsUrl() -> QUrl {
  return QUrl::fromLocalFile(QmlDirectory() +
                             QStringLiteral("/EditorAdjustmentTransferActions.qml"));
}

void ProcessEvents(int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

class EditorAdjustmentTransferActionsQmlHarness {
 public:
  RecordingAdjustmentTransfer transfer_fake;
  QQmlApplicationEngine       engine;
  QQuickWindow*               window = nullptr;
  QStringList                 warnings;

  EditorAdjustmentTransferActionsQmlHarness() {
    AppTheme::RegisterFonts();
    AppTheme::Instance().setReduceMotion(true);
    QQuickStyle::setStyle(QStringLiteral("Material"));

    QObject::connect(&engine, &QQmlEngine::warnings, [this](const QList<QQmlError>& emitted) {
      for (const auto& warning : emitted) {
        warnings.push_back(warning.toString());
      }
    });
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, [this](const QUrl& url) {
          warnings.push_back(QStringLiteral("Object creation failed: ") + url.toString());
        });
    engine.addImportPath(QStringLiteral("qrc:/"));
    engine.addImportPath(QmlDirectory());
    engine.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
    engine.rootContext()->setContextProperty(QStringLiteral("adjustmentTransferFake"),
                                             &transfer_fake);
    engine.rootContext()->setContextProperty(QStringLiteral("actionSourceUrl"), ActionsUrl());
    engine.loadData(QByteArray{kHarnessQml},
                    QUrl(QStringLiteral("file:///EditorAdjustmentTransferActionsHarness.qml")));
    if (!engine.rootObjects().empty()) {
      window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
      if (window) {
        window->show();
        window->requestActivate();
      }
    }
    ProcessEvents(20);
  }

  auto actions() const -> QObject* {
    return window ? window->findChild<QObject*>(QStringLiteral("editorAdjustmentTransferActions"))
                  : nullptr;
  }

  auto transfer() const -> QObject* {
    return const_cast<RecordingAdjustmentTransfer*>(&transfer_fake);
  }

  auto dialog() const -> QObject* {
    return window ? window->findChild<QObject*>(QStringLiteral("adjustmentTransferDialog"))
                  : nullptr;
  }

  auto policy() const -> QObject* {
    return window ? window->findChild<QObject*>(QStringLiteral("interactionPolicy")) : nullptr;
  }
};

void InvokeNoArg(QObject* object, const char* method) {
  ASSERT_NE(object, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(object, method, Qt::DirectConnection));
}

void InvokeStrategy(QObject* object, const QString& strategy) {
  ASSERT_NE(object, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(object, "applyPaste", Qt::DirectConnection,
                                        Q_ARG(QVariant, QVariant(strategy))));
}

TEST(EditorAdjustmentTransferActionsQmlTest, AllowedPasteOpensDialogAndCallsBackendWithTargets) {
  EditorAdjustmentTransferActionsQmlHarness harness;
  ASSERT_NE(harness.window, nullptr) << harness.warnings.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings.isEmpty()) << harness.warnings.join('\n').toStdString();
  auto* actions  = harness.actions();
  auto* transfer = harness.transfer();
  auto* dialog   = harness.dialog();
  ASSERT_NE(actions, nullptr);
  ASSERT_NE(transfer, nullptr);
  ASSERT_NE(dialog, nullptr);

  InvokeNoArg(actions, "requestPasteAdjustments");
  EXPECT_EQ(dialog->property("mode").toString(), QStringLiteral("paste"));
  EXPECT_EQ(dialog->property("pasteStrategy").toString(), QStringLiteral("merge"));
  EXPECT_EQ(dialog->property("sourceTitle").toString(), QStringLiteral("Copied source"));
  EXPECT_EQ(dialog->property("targetCount").toInt(), 2);
  EXPECT_EQ(dialog->property("openCount").toInt(), 1);

  InvokeStrategy(actions, QStringLiteral("merge"));
  EXPECT_EQ(harness.transfer_fake.paste_call_count(), 1);
  EXPECT_EQ(harness.transfer_fake.last_strategy(), QStringLiteral("merge"));
  EXPECT_EQ(harness.transfer_fake.last_targets().size(), 2);
}

TEST(EditorAdjustmentTransferActionsQmlTest, MissingPackageOrTargetsDoesNotOpenOrCallBackend) {
  EditorAdjustmentTransferActionsQmlHarness harness;
  ASSERT_NE(harness.window, nullptr) << harness.warnings.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings.isEmpty()) << harness.warnings.join('\n').toStdString();
  auto* actions  = harness.actions();
  auto* transfer = harness.transfer();
  auto* dialog   = harness.dialog();
  ASSERT_NE(actions, nullptr);
  ASSERT_NE(transfer, nullptr);
  ASSERT_NE(dialog, nullptr);

  transfer->setProperty("packageAvailable", false);
  ProcessEvents(10);
  InvokeNoArg(actions, "requestPasteAdjustments");
  EXPECT_EQ(dialog->property("openCount").toInt(), 0);

  transfer->setProperty("packageAvailable", true);
  actions->setProperty("pendingTargets", QVariantList{});
  ProcessEvents(10);
  InvokeNoArg(actions, "requestPasteAdjustments");
  InvokeStrategy(actions, QStringLiteral("merge"));
  EXPECT_EQ(dialog->property("openCount").toInt(), 0);
  EXPECT_EQ(harness.transfer_fake.paste_call_count(), 0);
}

TEST(EditorAdjustmentTransferActionsQmlTest, PolicyBlocksPasteAtOpenAndRecoversAfterUnlock) {
  EditorAdjustmentTransferActionsQmlHarness harness;
  ASSERT_NE(harness.window, nullptr) << harness.warnings.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings.isEmpty()) << harness.warnings.join('\n').toStdString();
  auto* actions  = harness.actions();
  auto* transfer = harness.transfer();
  auto* dialog   = harness.dialog();
  auto* policy   = harness.policy();
  ASSERT_NE(actions, nullptr);
  ASSERT_NE(transfer, nullptr);
  ASSERT_NE(dialog, nullptr);
  ASSERT_NE(policy, nullptr);

  policy->setProperty("canPasteAdjustments", false);
  ProcessEvents(10);
  InvokeNoArg(actions, "requestPasteAdjustments");
  EXPECT_EQ(dialog->property("openCount").toInt(), 0);
  EXPECT_EQ(harness.transfer_fake.paste_call_count(), 0);
  EXPECT_EQ(harness.window->property("lastMessage").toString(), QStringLiteral("Paste is blocked"));

  policy->setProperty("canPasteAdjustments", true);
  ProcessEvents(10);
  InvokeNoArg(actions, "requestPasteAdjustments");
  EXPECT_EQ(dialog->property("openCount").toInt(), 1);
}

TEST(EditorAdjustmentTransferActionsQmlTest,
     MergePolicyBlocksCommitWithoutBackendCallAndAllowsPaste) {
  EditorAdjustmentTransferActionsQmlHarness harness;
  ASSERT_NE(harness.window, nullptr) << harness.warnings.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings.isEmpty()) << harness.warnings.join('\n').toStdString();
  auto* actions  = harness.actions();
  auto* transfer = harness.transfer();
  auto* policy   = harness.policy();
  ASSERT_NE(actions, nullptr);
  ASSERT_NE(transfer, nullptr);
  ASSERT_NE(policy, nullptr);

  policy->setProperty("canMergeAdjustments", false);
  ProcessEvents(10);
  InvokeStrategy(actions, QStringLiteral("merge"));
  EXPECT_EQ(harness.transfer_fake.paste_call_count(), 0);
  EXPECT_EQ(harness.window->property("lastMessage").toString(), QStringLiteral("Merge is blocked"));

  InvokeStrategy(actions, QStringLiteral("paste"));
  EXPECT_EQ(harness.transfer_fake.paste_call_count(), 1);
  EXPECT_EQ(harness.transfer_fake.last_strategy(), QStringLiteral("paste"));
}

}  // namespace
}  // namespace alcedo::ui::test

#include "editor_adjustment_transfer_actions_qml_test.moc"
