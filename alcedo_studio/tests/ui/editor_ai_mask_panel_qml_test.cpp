//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// SM3.E (#537): the AI Mask panel shows the training data notice of the Sky model before the
// first Sky run (AI Models plan, decision A7 and section 4).
// AI Models plan, AM5: without a usable installed AI Mask model set the panel shows that state
// and "Open AI Models", which opens Settings and closes the tool.

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
#include <QMetaObject>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRectF>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVariantMap>
#include <filesystem>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/app_theme.hpp"

namespace alcedo::ui::test {
namespace {

auto QmlDirectory() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

void ProcessEvents(int milliseconds) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

auto WaitFor(const std::function<bool()>& condition, int timeout_ms = 2000) -> bool {
  QDeadlineTimer deadline(timeout_ms);
  while (!condition()) {
    if (deadline.hasExpired()) {
      return false;
    }
    ProcessEvents(20);
  }
  return true;
}

/// The first item named @p object_name in the visual tree below @p item.
auto FindVisualItem(QQuickItem* item, const QString& object_name) -> QQuickItem* {
  if (item == nullptr) {
    return nullptr;
  }
  if (item->objectName() == object_name) {
    return item;
  }
  for (auto* child : item->childItems()) {
    if (auto* found = FindVisualItem(child, object_name)) {
      return found;
    }
  }
  return nullptr;
}

/// Stand-in for EditorAiMaskTool after an Open that the installed model set stopped.
constexpr auto kStoppedToolQml = R"(
import QtQuick
QtObject {
    property bool active: true
    property bool ready: false
    property bool busy: false
    property string busyStage: ""
    property int busyStep: 0
    property int busyStepCount: 0
    property string errorText: ""
    property string noticeText: ""
    property string modelSetState: "notInstalled"
    property bool editingStoredMask: false
    property bool subtract: false
    property string promptTool: "point"
    property bool canUndo: false
    property bool canRedo: false
    property real featherPx: 0
    property int expandPx: 0
    property real maximumFeatherPx: 50
    property int maximumExpandPx: 50
    property int levelCount: 0
    property int selectedLevel: 0
    property var people: []
    property bool peopleActive: false
    property string previewStatus: ""
    property string previewError: ""
    property int cancelCount: 0
    function cancel() { cancelCount += 1 }
}
)";

/// Stand-in for the application host (Main.qml).
constexpr auto kHostQml        = R"(
import QtQuick
QtObject {
    property int aiModelSettingsOpenCount: 0
    function openAiModelSettings() { aiModelSettingsOpenCount += 1 }
}
)";

/// The production AI Mask panel. Without @p with_stopped_tool it has no tool: the state before
/// any task runs.
class AiMaskPanelHarness {
 public:
  explicit AiMaskPanelHarness(bool with_stopped_tool = false) {
    static bool registered = false;
    if (!registered) {
      RegisterEditorAdjustmentQmlTypes();
      registered = true;
    }
    AppTheme::RegisterFonts();
    AppTheme::Instance().setReduceMotion(true);
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    engine_.addImportPath(QStringLiteral("qrc:/"));
    engine_.addImportPath(QmlDirectory());
    engine_.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());

    QVariantMap properties;
    if (with_stopped_tool) {
      tool_ = CreateStandIn(kStoppedToolQml);
      host_ = CreateStandIn(kHostQml);
      if (!tool_ || !host_) {
        return;
      }
      properties.insert(QStringLiteral("tool"), QVariant::fromValue<QObject*>(tool_.get()));
      properties.insert(QStringLiteral("host"), QVariant::fromValue<QObject*>(host_.get()));
    }
    QQmlComponent component(
        &engine_, QUrl::fromLocalFile(QmlDirectory() + QStringLiteral("/EditorAiMaskPanel.qml")));
    if (component.isError()) {
      errors_ = component.errors();
      return;
    }
    root_.reset(qobject_cast<QQuickItem*>(component.createWithInitialProperties(properties)));
    if (!root_) {
      errors_ = component.errors();
      return;
    }
    root_->setWidth(320);
    root_->setHeight(760);
    root_->setParentItem(window_.contentItem());
    window_.resize(320, 760);
    window_.show();
    ProcessEvents(100);
  }

  [[nodiscard]] auto root() const -> QQuickItem* { return root_.get(); }
  [[nodiscard]] auto tool() const -> QObject* { return tool_.get(); }
  [[nodiscard]] auto host() const -> QObject* { return host_.get(); }
  [[nodiscard]] auto errors() const -> QString {
    QStringList text;
    for (const auto& error : errors_) {
      text.push_back(error.toString());
    }
    return text.join('\n') + stand_in_error_;
  }

 private:
  auto CreateStandIn(const char* qml) -> std::unique_ptr<QObject> {
    QQmlComponent component(&engine_);
    component.setData(QByteArray(qml), QUrl());
    std::unique_ptr<QObject> object(component.create());
    if (!object) {
      errors_         = component.errors();
      stand_in_error_ = component.errorString();
    }
    return object;
  }

  QQmlEngine                  engine_;
  std::unique_ptr<QObject>    tool_;
  std::unique_ptr<QObject>    host_;
  QQuickWindow                window_;
  std::unique_ptr<QQuickItem> root_;
  QList<QQmlError>            errors_;
  QString                     stand_in_error_;
};

TEST(EditorAiMaskPanelQmlTest, SkyTrainingDataNoticeIsVisibleBeforeFirstSkyRun) {
  AiMaskPanelHarness harness;
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  QQuickItem* notice = nullptr;
  ASSERT_TRUE(WaitFor([&] {
    notice = FindVisualItem(harness.root(), QStringLiteral("editorAiMaskSkyNotice"));
    return notice != nullptr && notice->isVisible() && notice->height() > 0.0;
  })) << "the Sky notice is missing or hidden";

  // AI Models plan, section 4: the license is not clear, the user decides, and Settings > AI
  // Models has the statement. The notice does not forbid commercial use (decision A7).
  const auto text = notice->property("text").toString();
  EXPECT_EQ(text, QStringLiteral("Sky: the license of the Sky model's training data is not clear. "
                                 "Decide yourself before commercial use; see Settings > AI "
                                 "Models."));
  EXPECT_FALSE(text.contains(QStringLiteral("Do not use"))) << text.toStdString();

  // The notice is in the panel area that shows without a scroll, next to the Sky task button.
  auto* sky = FindVisualItem(harness.root(), QStringLiteral("editorAiMaskSky"));
  ASSERT_NE(sky, nullptr);
  const QRectF panel(0.0, 0.0, harness.root()->width(), harness.root()->height());
  const QRectF notice_rect =
      notice->mapRectToItem(harness.root(), QRectF(0.0, 0.0, notice->width(), notice->height()));
  EXPECT_TRUE(panel.contains(notice_rect)) << notice_rect.x() << "," << notice_rect.y() << " "
                                           << notice_rect.width() << "x" << notice_rect.height();
  const QRectF sky_rect =
      sky->mapRectToItem(harness.root(), QRectF(0.0, 0.0, sky->width(), sky->height()));
  EXPECT_GE(notice_rect.top(), sky_rect.bottom());
  EXPECT_LT(notice_rect.top() - sky_rect.bottom(), 40.0);
}

TEST(EditorAiMaskPanelQmlTest, MissingModelSetShowsItsStateAndOpenAiModelsOpensSettings) {
  AiMaskPanelHarness harness(true);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  QQuickItem* text = nullptr;
  ASSERT_TRUE(WaitFor([&] {
    text = FindVisualItem(harness.root(), QStringLiteral("editorAiMaskModelSetText"));
    return text != nullptr && text->isVisible() && text->height() > 0.0;
  })) << "the model set state is missing or hidden";
  EXPECT_EQ(text->property("text").toString(),
            QStringLiteral("The AI Mask models are not installed. Download them on the AI Models "
                           "page in Settings."));

  // Only the state shows: no task, no Confirm.
  auto* subject = FindVisualItem(harness.root(), QStringLiteral("editorAiMaskSubject"));
  ASSERT_NE(subject, nullptr);
  EXPECT_FALSE(subject->isVisible());
  auto* commit = FindVisualItem(harness.root(), QStringLiteral("editorAiMaskCommit"));
  ASSERT_NE(commit, nullptr);
  EXPECT_FALSE(commit->isVisible());

  // Each state has its own text.
  const std::vector<std::pair<QString, QString>> states = {
      {QStringLiteral("updateRequired"), QStringLiteral("too old")},
      {QStringLiteral("needsNewerApplication"),
       QStringLiteral("need a newer version of Alcedo Studio")},
      {QStringLiteral("repairRequired"), QStringLiteral("incomplete")}};
  for (const auto& [state, expected] : states) {
    harness.tool()->setProperty("modelSetState", state);
    ProcessEvents(20);
    EXPECT_TRUE(text->property("text").toString().contains(expected)) << state.toStdString();
  }

  auto* open = FindVisualItem(harness.root(), QStringLiteral("editorAiMaskOpenAiModels"));
  ASSERT_NE(open, nullptr);
  ASSERT_TRUE(open->isVisible());
  ASSERT_TRUE(QMetaObject::invokeMethod(open, "clicked"));
  ProcessEvents(20);
  EXPECT_EQ(harness.host()->property("aiModelSettingsOpenCount").toInt(), 1)
      << "Settings opens on the AI Models page";
  EXPECT_EQ(harness.tool()->property("cancelCount").toInt(), 1) << "the tool closes";
}

}  // namespace
}  // namespace alcedo::ui::test
