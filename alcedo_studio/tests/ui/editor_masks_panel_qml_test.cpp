//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// #564 (RP3): the Mask panel has two parts. The Luminance/Hue Range section is one instance on
// top, outside the scrolling lower part; the AI Mask panel replaces only the lower part while its
// edit is open. #605: the section is collapsed by default and keeps the user's choice. Fake
// QtObjects stand in for EditorMaskCreationAdapter and EditorAiMaskTool: this test checks the panel
// layout only (the adapter tests check the edits).

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

/// Click the header of the CollapsibleSection @p section (the user expands or collapses it).
void ToggleSection(QQuickItem* section) {
  QMetaObject::invokeMethod(section, "toggle");
  ProcessEvents(20);
}

auto SectionExpanded(QQuickItem* section) -> bool { return section->property("expanded").toBool(); }

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

// Main.qml keeps the expanded state of the Range section for the session (#605).
constexpr auto kFakeHost         = R"(import QtQuick
QtObject {
    property bool editorMaskRangeSectionExpanded: false
})";

/// The production Mask panel with fake owners; with @p with_host also a fake application host.
class MasksPanelHarness {
 public:
  explicit MasksPanelHarness(bool with_host = false) {
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
    if (with_host) {
      host_.reset(Create(kFakeHost));
    }
    window_.resize(320, 900);
    window_.show();
    CreatePanel();
  }

  /// Destroy the panel and create a new one with the same owners (the editor Loader recreates it).
  void CreatePanel() {
    root_.reset();
    QQmlComponent component(
        &engine_,
        QUrl::fromLocalFile(QmlDirectory() + QStringLiteral("/EditorMasksContextPanel.qml")));
    QVariantMap properties{
        {QStringLiteral("maskCreation"), QVariant::fromValue(mask_creation_.get())},
        {QStringLiteral("aiMaskTool"), QVariant::fromValue(ai_mask_tool_.get())}};
    if (host_) {
      properties.insert(QStringLiteral("host"), QVariant::fromValue(host_.get()));
    }
    root_.reset(qobject_cast<QQuickItem*>(component.createWithInitialProperties(properties)));
    if (!root_) {
      for (const auto& error : component.errors()) {
        warnings_.push_back(error.toString());
      }
      return;
    }
    root_->setWidth(320);
    root_->setHeight(900);
    root_->setParentItem(window_.contentItem());
    ProcessEvents(100);
  }

  [[nodiscard]] auto root() const -> QQuickItem* { return root_.get(); }
  [[nodiscard]] auto ai_mask_tool() const -> QObject* { return ai_mask_tool_.get(); }
  [[nodiscard]] auto mask_creation() const -> QObject* { return mask_creation_.get(); }
  [[nodiscard]] auto host() const -> QObject* { return host_.get(); }
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
  std::unique_ptr<QObject>    host_;
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
           range_shell->isVisible();
  })) << "the Range section of the selected Radial Mask is missing";
  ToggleSection(range_shell);
  ASSERT_TRUE(WaitFor([&] { return range_shell->height() > 100.0; }))
      << "the expanded Range section is empty";

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
  // stays above it, and only one Range section exists.
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

  // #604: the range controls of the new candidate are enabled while the tool is ready, without
  // the old "Confirm the new AI Mask" notice, and disabled while an AI operation runs.
  EXPECT_EQ(FindVisualItem(harness.root(), QStringLiteral("editorMaskRangeCandidateNotice")),
            nullptr);
  for (const auto* name : {"editorMaskLuminanceRange", "editorMaskHueRange",
                           "editorMaskLuminanceLowField", "editorMaskRangeShowSelection"}) {
    auto* item = FindVisualItem(range_shell, QString::fromLatin1(name));
    ASSERT_NE(item, nullptr) << name;
    EXPECT_TRUE(WaitFor([&] { return item->isEnabled(); })) << name << " is disabled";
  }
  auto* luminance_bar = FindVisualItem(range_shell, QStringLiteral("editorMaskLuminanceRange"));
  harness.ai_mask_tool()->setProperty("busy", true);
  EXPECT_TRUE(WaitFor([&] { return !luminance_bar->isEnabled(); }))
      << "the range controls stay enabled while an AI operation runs";
  harness.ai_mask_tool()->setProperty("busy", false);
  EXPECT_TRUE(WaitFor([&] { return luminance_bar->isEnabled(); }));

  EXPECT_FALSE(QString::fromStdString(harness.warnings()).contains(QStringLiteral("ReferenceError")))
      << harness.warnings();
}

// #605, #606: the Luminance/Hue Range section is collapsed when the Mask panel first shows a
// selected Mask and when an AI Mask edit opens. The user's choice stays for the session: selection
// changes, an AI Mask edit, and a new panel (editor Loader) do not change it.
TEST(EditorMasksPanelQmlTest, RangeSectionIsCollapsedByDefaultAndKeepsTheUserChoice) {
  MasksPanelHarness harness(true);
  ASSERT_NE(harness.root(), nullptr) << harness.warnings();
  QQuickItem* range_shell = nullptr;
  ASSERT_TRUE(WaitFor([&] {
    range_shell = FindVisualItem(harness.root(), QStringLiteral("editorMasksRangeShell"));
    return range_shell != nullptr && range_shell->isVisible();
  }));
  EXPECT_EQ(range_shell->property("title").toString(), QStringLiteral("Luminance/Hue Range"));
  EXPECT_FALSE(SectionExpanded(range_shell)) << "a selected Mask expands the Range section";
  const double collapsed_height = range_shell->height();
  EXPECT_LT(collapsed_height, 60.0) << "the collapsed section shows its controls";

  // An AI Mask edit does not expand it.
  harness.ai_mask_tool()->setProperty("active", true);
  ASSERT_TRUE(WaitFor([&] {
    auto* ai_panel = FindVisualItem(harness.root(), QStringLiteral("editorAiMaskPanel"));
    return ai_panel != nullptr && ai_panel->isVisible();
  }));
  EXPECT_FALSE(SectionExpanded(range_shell)) << "an AI Mask edit expands the Range section";
  harness.ai_mask_tool()->setProperty("active", false);

  // The user expands it: it stays expanded through selection changes and an AI Mask edit.
  ToggleSection(range_shell);
  ASSERT_TRUE(SectionExpanded(range_shell));
  EXPECT_TRUE(harness.host()->property("editorMaskRangeSectionExpanded").toBool());
  ASSERT_TRUE(WaitFor([&] { return range_shell->height() > 100.0; }));
  harness.mask_creation()->setProperty("selectedMaskId", QString());
  ProcessEvents(20);
  harness.mask_creation()->setProperty("selectedMaskId", QStringLiteral("mask.ai"));
  harness.mask_creation()->setProperty("toolKind", QStringLiteral("aiSegmentation"));
  ProcessEvents(20);
  EXPECT_TRUE(SectionExpanded(range_shell)) << "a selection change collapses the section";
  harness.ai_mask_tool()->setProperty("active", true);
  ProcessEvents(20);
  EXPECT_TRUE(SectionExpanded(range_shell)) << "an AI Mask edit collapses the section";
  harness.ai_mask_tool()->setProperty("active", false);

  // A new panel (the editor Loader recreates it) shows the session state.
  harness.CreatePanel();
  ASSERT_NE(harness.root(), nullptr) << harness.warnings();
  range_shell = FindVisualItem(harness.root(), QStringLiteral("editorMasksRangeShell"));
  ASSERT_NE(range_shell, nullptr);
  EXPECT_TRUE(SectionExpanded(range_shell)) << "the session state is lost with the panel";

  // The user collapses it again; the next panel starts collapsed.
  ToggleSection(range_shell);
  EXPECT_FALSE(SectionExpanded(range_shell));
  EXPECT_FALSE(harness.host()->property("editorMaskRangeSectionExpanded").toBool());
  harness.CreatePanel();
  ASSERT_NE(harness.root(), nullptr) << harness.warnings();
  range_shell = FindVisualItem(harness.root(), QStringLiteral("editorMasksRangeShell"));
  ASSERT_NE(range_shell, nullptr);
  EXPECT_FALSE(SectionExpanded(range_shell));

  // The host is the owner of the state: a change of it is shown.
  harness.host()->setProperty("editorMaskRangeSectionExpanded", true);
  EXPECT_TRUE(WaitFor([&] { return SectionExpanded(range_shell); }));
  EXPECT_FALSE(
      QString::fromStdString(harness.warnings()).contains(QStringLiteral("ReferenceError")))
      << harness.warnings();
}

// #605: an AI Mask edit that opens with the panel shows the Range section collapsed.
TEST(EditorMasksPanelQmlTest, RangeSectionIsCollapsedWhenAnAiMaskEditOpens) {
  MasksPanelHarness harness;
  ASSERT_NE(harness.root(), nullptr) << harness.warnings();
  harness.mask_creation()->setProperty("selectedMaskId", QString());
  harness.ai_mask_tool()->setProperty("active", true);
  QQuickItem* range_shell = nullptr;
  ASSERT_TRUE(WaitFor([&] {
    range_shell = FindVisualItem(harness.root(), QStringLiteral("editorMasksRangeShell"));
    return range_shell != nullptr && range_shell->isVisible();
  }));
  EXPECT_FALSE(SectionExpanded(range_shell));
  EXPECT_LT(range_shell->height(), 60.0);
}

// #607: the Softness and Saturation Threshold sliders are enabled for a full range.
TEST(EditorMasksPanelQmlTest, RangeParameterSlidersAreEnabledForAFullRange) {
  MasksPanelHarness harness;
  ASSERT_NE(harness.root(), nullptr) << harness.warnings();
  auto* range_shell = FindVisualItem(harness.root(), QStringLiteral("editorMasksRangeShell"));
  ASSERT_NE(range_shell, nullptr);
  ToggleSection(range_shell);
  // The fake owner has full ranges (0..1, 0..360).
  for (const auto* name :
       {"editorMaskLuminanceSoftness", "editorMaskHueSoftness", "editorMaskHueMinChroma"}) {
    auto* slider = FindVisualItem(range_shell, QString::fromLatin1(name));
    ASSERT_NE(slider, nullptr) << name;
    auto* handle = FindVisualItem(slider, QStringLiteral("adjustmentSliderHandle"));
    ASSERT_NE(handle, nullptr) << name;
    EXPECT_TRUE(WaitFor([&] { return handle->isEnabled(); })) << name << " is disabled";
  }
  // Not while the range section is unavailable.
  harness.mask_creation()->setProperty("maskRangeAvailable", false);
  auto* slider = FindVisualItem(range_shell, QStringLiteral("editorMaskHueSoftness"));
  auto* handle = FindVisualItem(slider, QStringLiteral("adjustmentSliderHandle"));
  EXPECT_TRUE(WaitFor([&] { return !handle->isEnabled(); }));
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
