//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <QVariantMap>
#include <filesystem>

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

// Harness window creates the real AdjustmentTransferDialog in paste mode with a
// sample parameter row, opens it on the window overlay, and exposes it as the
// `dialog` property. A Dialog needs an Overlay (window) to parent to, so the
// dialog is created against this ApplicationWindow, not a bare QtObject.
constexpr char kHarnessQml[] = R"(
import QtQuick
import QtQuick.Controls.Basic

ApplicationWindow {
    id: root
    objectName: "adjustmentTransferDialogHarness"
    width: 720
    height: 600
    visible: true
    property var dialog: null
    property string createError: ""
    property var sampleRows: [
        {"key": "exposure", "label": "Exposure", "section": "Tone",
         "value": "+1.0", "checked": true}
    ]

    Component.onCompleted: {
        var comp = Qt.createComponent(dialogComponentUrl)
        if (comp.status === Component.Error) {
            root.createError = comp.errorString()
            return
        }
        root.dialog = comp.createObject(root, {
            "mode": "paste",
            "pasteStrategy": "paste",
            "sourceTitle": "Copied source",
            "targetCount": 2,
            "adjustmentRows": root.sampleRows,
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

void ProcessEvents(int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

class AdjustmentTransferDialogQmlHarness {
 public:
  QQmlApplicationEngine engine;
  QQuickWindow*         window = nullptr;
  QStringList           warnings;

  AdjustmentTransferDialogQmlHarness() {
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
    engine.loadData(QByteArray{kHarnessQml},
                    QUrl(QStringLiteral("file:///AdjustmentTransferDialogHarness.qml")));
    if (!engine.rootObjects().empty()) {
      window = qobject_cast<QQuickWindow*>(engine.rootObjects().front());
      if (window) {
        window->show();
        window->requestActivate();
      }
    }
    ProcessEvents(80);
  }

  auto dialog() const -> QObject* {
    return window ? window->property("dialog").value<QObject*>() : nullptr;
  }

  auto createError() const -> QString {
    return window ? window->property("createError").toString() : QString{};
  }

  auto findChildByName(const QString& name) const -> QObject* {
    if (!window) return nullptr;
    auto* d = dialog();
    if (d) {
      auto* child = d->findChild<QObject*>(name);
      if (child) return child;
    }
    return window->findChild<QObject*>(name);
  }
};

TEST(AdjustmentTransferDialogQmlTest, LoadsInPasteModeWithPasteAcceptLabel) {
  AdjustmentTransferDialogQmlHarness harness;
  ASSERT_NE(harness.window, nullptr) << harness.warnings.join('\n').toStdString();
  ASSERT_TRUE(harness.warnings.isEmpty()) << harness.warnings.join('\n').toStdString();

  auto* dialog = harness.dialog();
  ASSERT_NE(dialog, nullptr) << harness.warnings.join("\n").toStdString()
                            << "| createError=" << harness.createError().toStdString();
  EXPECT_EQ(dialog->property("mode").toString(), QStringLiteral("paste"));
  EXPECT_EQ(harness.findChildByName(QStringLiteral("adjustmentTransferStrategySwitcher")),
            nullptr);
  EXPECT_EQ(harness.findChildByName(QStringLiteral("adjustmentTransferMergeNotice")), nullptr);

  auto* accept = harness.findChildByName(QStringLiteral("adjustmentTransferAcceptButton"));
  ASSERT_NE(accept, nullptr);
  EXPECT_TRUE(accept->property("text").toString().contains(QStringLiteral("Paste")));
  EXPECT_FALSE(accept->property("text").toString().contains(QStringLiteral("Merge")));
}

TEST(AdjustmentTransferDialogQmlTest, TransferSurfaceHasNoPipelineMergeOperation) {
  AdjustmentTransferDialogQmlHarness harness;
  ASSERT_NE(harness.dialog(), nullptr)
      << harness.warnings.join("\n").toStdString()
      << "| createError=" << harness.createError().toStdString();
  auto* dialog = harness.dialog();
  dialog->setProperty("pasteStrategy", QStringLiteral("merge"));
  ProcessEvents(40);
  auto* accept = harness.findChildByName(QStringLiteral("adjustmentTransferAcceptButton"));
  ASSERT_NE(accept, nullptr);
  EXPECT_TRUE(accept->property("text").toString().contains(QStringLiteral("Paste")));
  EXPECT_FALSE(accept->property("text").toString().contains(QStringLiteral("Merge")));
}

}  // namespace
}  // namespace alcedo::ui::test