//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// #564 (RP3): the Mask panel has two parts. The Range section is one instance on top, outside
// the scrolling lower part; the AI Mask panel replaces only the lower part while its edit is
// open. Fake QtObjects stand in for EditorMaskCreationAdapter and EditorAiMaskTool: this test
// checks the panel layout only (the adapter tests check the edits).

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QEventLoop>
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

void CollectVisualItems(QQuickItem* item, const QString& object_name, QList<QQuickItem*>& found) {
  if (item == nullptr) {
    return;
  }
  if (item->objectName() == object_name) {
    found.push_back(item);
  }
  for (auto* child : item->childItems()) {
    CollectVisualItems(child, object_name, found);
  }
}

auto FindVisualItem(QQuickItem* item, const QString& object_name) -> QQuickItem* {
  QList<QQuickItem*> found;
  CollectVisualItems(item, object_name, found);
  return found.isEmpty() ? nullptr : found.front();
}

auto IsDescendant(QQuickItem* item, QQuickItem* ancestor) -> bool {
  for (auto* parent = item != nullptr ? item->parentItem() : nullptr; parent != nullptr;
       parent = parent->parentItem()) {
    if (parent == ancestor) {
      return true;
    }
  }
  return false;
}

auto SceneRect(QQuickItem* item) -> QRectF {
  return item->mapRectToScene(QRectF(0.0, 0.0, item->width(), item->height()));
}

constexpr auto kFakeMaskCreation = R"(import QtQuick
QtObject {
    property string selectedMaskId: "mask.radial"
    property string selectedMaskNodeId: "grade.primary"
    property string toolKind: "radial"
    property bool creating: false
    property bool maskControlsActive: true
    property bool maskRangeAvailable: true
    property real maskLuminanceLow: 0
    property real maskLuminanceHigh: 1
    property real maskHueStart: 0
    property real maskHueEnd: 360
    property real maskLuminanceSoftness: 0.05
    property real maskHueSoftness: 12
    property real maskHueMinChroma: 0.03
    property bool maskRangeCoverageVisible: false
    property real luminanceRangeMinimumWidth: 0.012
    property real hueRangeMinimumWidth: 1.2
    property var luminanceGradientStops: []
    property var hueGradientStops: []
    property var rangeParameterLimits: ({})
    property string maskName: "Radial"
    property bool maskEnabled: true
    property bool maskInvert: false
    property real maskOpacityPercent: 100
    property bool maskNudgeAvailable: false
    signal maskCreationChanged()
})";

constexpr auto kFakeAiMaskTool   = R"(import QtQuick
QtObject {
    property bool active: false
    property bool ready: true
    property bool busy: false
    property bool editingStoredMask: false
    property int openSelectedMaskCalls: 0
    function openSelectedMask() { openSelectedMaskCalls += 1 }
})";

/// The production Mask panel with fake owners.
class MasksPanelHarness {
 public:
  MasksPanelHarness() {
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
    QObject::connect(&engine_, &QQmlEngine::warnings, [this](const QList<QQmlError>& warnings) {
      for (const auto& warning : warnings) {
        warnings_.push_back(warning.toString());
      }
    });

    mask_creation_.reset(Create(kFakeMaskCreation));
    ai_mask_tool_.reset(Create(kFakeAiMaskTool));
    QQmlComponent component(
        &engine_,
        QUrl::fromLocalFile(QmlDirectory() + QStringLiteral("/EditorMasksContextPanel.qml")));
    root_.reset(qobject_cast<QQuickItem*>(component.createWithInitialProperties(
        {{QStringLiteral("maskCreation"), QVariant::fromValue(mask_creation_.get())},
         {QStringLiteral("aiMaskTool"), QVariant::fromValue(ai_mask_tool_.get())}})));
    if (!root_) {
      for (const auto& error : component.errors()) {
        warnings_.push_back(error.toString());
      }
      return;
    }
    root_->setWidth(320);
    root_->setHeight(900);
    root_->setParentItem(window_.contentItem());
    window_.resize(320, 900);
    window_.show();
    ProcessEvents(100);
  }

  [[nodiscard]] auto root() const -> QQuickItem* { return root_.get(); }
  [[nodiscard]] auto ai_mask_tool() const -> QObject* { return ai_mask_tool_.get(); }
  [[nodiscard]] auto mask_creation() const -> QObject* { return mask_creation_.get(); }
  [[nodiscard]] auto warnings() const -> std::string {
    return warnings_.join('\n').toStdString();
  }

 private:
  auto Create(const char* source) -> QObject* {
    QQmlComponent component(&engine_);
    component.setData(source, QUrl());
    return component.create();
  }

  QQmlEngine                  engine_;
  QQuickWindow                window_;
  std::unique_ptr<QObject>    mask_creation_;
  std::unique_ptr<QObject>    ai_mask_tool_;
  std::unique_ptr<QQuickItem> root_;
  QStringList                 warnings_;
};

TEST(EditorMasksPanelQmlTest, RangeSectionIsOneFixedUpperPartAndTheAiPanelReplacesTheLowerPart) {
  MasksPanelHarness harness;
  ASSERT_NE(harness.root(), nullptr) << harness.warnings();

  QQuickItem* range_shell = nullptr;
  QQuickItem* lower       = nullptr;
  QQuickItem* scroll      = nullptr;
  ASSERT_TRUE(WaitFor([&] {
    range_shell = FindVisualItem(harness.root(), QStringLiteral("editorMasksRangeShell"));
    lower       = FindVisualItem(harness.root(), QStringLiteral("editorMasksLowerPart"));
    scroll      = FindVisualItem(harness.root(), QStringLiteral("editorMasksScroll"));
    return range_shell != nullptr && lower != nullptr && scroll != nullptr &&
           range_shell->isVisible() && range_shell->height() > 100.0;
  })) << "the Range section of the selected Radial Mask is missing or empty";

  // Selected Radial Mask: the Range section is above the lower part and not in its Flickable.
  EXPECT_FALSE(IsDescendant(range_shell, scroll)) << "the Range section scrolls with the fields";
  EXPECT_LE(SceneRect(range_shell).bottom(), SceneRect(lower).top() + 0.5);
  EXPECT_TRUE(scroll->isVisible());
  auto* name_field = FindVisualItem(harness.root(), QStringLiteral("editorMasksNameField"));
  ASSERT_NE(name_field, nullptr);
  EXPECT_TRUE(IsDescendant(name_field, scroll)) << "the Mask fields are in the lower part";
  for (const auto* name : {"editorMaskLuminanceLowField", "editorMaskLuminanceHighField",
                           "editorMaskHueStartField", "editorMaskHueEndField",
                           "editorMaskLuminanceSoftness", "editorMaskHueSoftness",
                           "editorMaskHueMinChroma", "editorMaskRangeShowSelection"}) {
    auto* item = FindVisualItem(range_shell, QString::fromLatin1(name));
    ASSERT_NE(item, nullptr) << name;
    EXPECT_TRUE(item->isVisible()) << name;
  }

  // The AI Mask edit of a new candidate: the AI panel fills the lower part, the Range section
  // stays above it with the candidate notice, and only one Range section exists.
  harness.ai_mask_tool()->setProperty("active", true);
  QQuickItem* ai_panel = nullptr;
  ASSERT_TRUE(WaitFor([&] {
    ai_panel = FindVisualItem(harness.root(), QStringLiteral("editorAiMaskPanel"));
    return ai_panel != nullptr && ai_panel->isVisible();
  }));
  EXPECT_TRUE(IsDescendant(ai_panel, lower));
  EXPECT_FALSE(scroll->isVisible());
  EXPECT_TRUE(range_shell->isVisible());
  EXPECT_LE(SceneRect(range_shell).bottom(), SceneRect(ai_panel).top() + 0.5);
  auto* confirm = FindVisualItem(harness.root(), QStringLiteral("editorMasksConfirm"));
  ASSERT_NE(confirm, nullptr);
  EXPECT_FALSE(confirm->isVisible()) << "only the AI Mask panel Confirm shows during its edit";
  QList<QQuickItem*> sections;
  CollectVisualItems(harness.root(), QStringLiteral("editorMasksRangeSection"), sections);
  CollectVisualItems(harness.root(), QStringLiteral("editorAiMaskRangeSection"), sections);
  EXPECT_EQ(sections.size(), 1);
  auto* notice = FindVisualItem(range_shell, QStringLiteral("editorMaskRangeCandidateNotice"));
  ASSERT_NE(notice, nullptr);
  EXPECT_TRUE(notice->isVisible());

  // The edit of a stored AI Mask: no candidate notice.
  harness.ai_mask_tool()->setProperty("editingStoredMask", true);
  EXPECT_TRUE(WaitFor([&] { return !notice->isVisible(); }));

  EXPECT_FALSE(QString::fromStdString(harness.warnings()).contains(QStringLiteral("ReferenceError")))
      << harness.warnings();
}

// #564 (RP4): a selected stored AI Mask without an open edit shows the Range section and an
// "Edit AI Mask" button in the lower part; only the button opens the edit.
TEST(EditorMasksPanelQmlTest, SelectedAiMaskShowsTheEditAiMaskButtonThatOpensItsEdit) {
  MasksPanelHarness harness;
  ASSERT_NE(harness.root(), nullptr) << harness.warnings();
  harness.mask_creation()->setProperty("selectedMaskId", QStringLiteral("mask.ai"));
  harness.mask_creation()->setProperty("toolKind", QStringLiteral("aiSegmentation"));

  QQuickItem* button = nullptr;
  ASSERT_TRUE(WaitFor([&] {
    button = FindVisualItem(harness.root(), QStringLiteral("editorMasksEditAiMask"));
    return button != nullptr && button->isVisible() && button->height() > 0.0;
  })) << "the Edit AI Mask button is missing or hidden";
  auto* range_shell = FindVisualItem(harness.root(), QStringLiteral("editorMasksRangeShell"));
  ASSERT_NE(range_shell, nullptr);
  EXPECT_TRUE(range_shell->isVisible());
  EXPECT_EQ(harness.ai_mask_tool()->property("openSelectedMaskCalls").toInt(), 0)
      << "a selection alone does not open the AI Mask edit";
  // The analytic source fields are not shown for an AI Mask.
  for (const auto* name : {"editorMasksInnerFeatherSlider", "editorMasksMajorRadiusSlider",
                           "editorMasksTransitionSlider", "editorMasksNudgeArea"}) {
    auto* item = FindVisualItem(harness.root(), QString::fromLatin1(name));
    EXPECT_TRUE(item == nullptr || !item->isVisible()) << name;
  }

  QMetaObject::invokeMethod(button, "clicked");
  ProcessEvents(20);
  EXPECT_EQ(harness.ai_mask_tool()->property("openSelectedMaskCalls").toInt(), 1);

  // While the edit is open, the AI Mask panel replaces the button.
  harness.ai_mask_tool()->setProperty("active", true);
  EXPECT_TRUE(WaitFor([&] { return !button->isVisible(); }));
}

}  // namespace
}  // namespace alcedo::ui::test
