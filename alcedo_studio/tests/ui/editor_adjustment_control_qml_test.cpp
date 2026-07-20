//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Phase 6A component tests for the shared QML adjustment controls. Loads
// AdjustmentSlider / AdjustmentToggle / AdjustmentCombo from source via an inline
// harness (mirrors GlobalSearchDialogQmlTest). The models are constructed in C++
// with a RecordingSubmitter wired in and exposed as context properties, so the
// test exercises the real control→model→submitter path without the Alcedo.Main
// QML plugin. QTest drives keyboard entry, reset/toggle clicks, and combo
// activation; QSignalSpy catches the debounced settled commit through the event
// loop. No GPU is required (offscreen QPA).

#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_submitter.hpp"
#include "ui/alcedo_main/app_theme.hpp"

#include <QApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointF>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QString>
#include <QStringList>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <vector>

namespace alcedo::ui::test {
namespace {

// Recording IEditorAdjustmentSubmitter fake (QObject so setSubmitter's
// dynamic_cast resolves it; no Q_OBJECT — no signals/slots/properties).
class RecordingSubmitter : public QObject, public IEditorAdjustmentSubmitter {
 public:
  struct Call {
    QString fieldKey;
    QString params;
    bool    settled;
  };
  std::vector<Call> calls;
  bool              canEditState = true;

  auto submitPatch(QString fieldKey, QString paramsJson, bool settled) -> bool override {
    if (!canEditState) {
      return false;
    }
    calls.push_back({fieldKey, paramsJson, settled});
    return true;
  }
  auto canEdit() const -> bool override { return canEditState; }

  auto settledCount() const -> int {
    return static_cast<int>(std::count_if(calls.begin(), calls.end(),
                                           [](const Call& c) { return c.settled; }));
  }
  auto interactiveCount() const -> int {
    return static_cast<int>(std::count_if(calls.begin(), calls.end(),
                                          [](const Call& c) { return !c.settled; }));
  }
  auto settledForField(const QString& field) const -> int {
    return static_cast<int>(std::count_if(
        calls.begin(), calls.end(), [&](const Call& c) { return c.settled && c.fieldKey == field; }));
  }
  static auto numericValue(const QString& params) -> double {
    return QJsonDocument::fromJson(params.toUtf8()).object().value("value").toDouble();
  }
  static auto boolValue(const QString& params) -> bool {
    return QJsonDocument::fromJson(params.toUtf8()).object().value("value").toBool();
  }
};

auto SrcQmlDir() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

auto SliderQmlUrl() -> QUrl {
  return QUrl::fromLocalFile(SrcQmlDir() + QStringLiteral("/AdjustmentSlider.qml"));
}
auto ToggleQmlUrl() -> QUrl {
  return QUrl::fromLocalFile(SrcQmlDir() + QStringLiteral("/AdjustmentToggle.qml"));
}
auto ComboQmlUrl() -> QUrl {
  return QUrl::fromLocalFile(SrcQmlDir() + QStringLiteral("/AdjustmentCombo.qml"));
}

void ProcessEvents(int ms) {
  QEventLoop loop;
  QTimer::singleShot(ms, &loop, &QEventLoop::quit);
  loop.exec();
}

// QTest::keyClicks has no QWindow* overload (only QWidget*), so type a string
// one character at a time via keyClick, which does have a QWindow* overload.
void TypeText(QQuickWindow* window, const QString& text) {
  for (const QChar& ch : text) {
    QTest::keyClick(window, static_cast<Qt::Key>(ch.unicode()));
  }
}

auto WaitUntil(const std::function<bool()>& predicate, int timeoutMs, int stepMs = 50) -> bool {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    ProcessEvents(stepMs);
  }
  return predicate();
}

// One harness instantiation of the three controls. The controls are loaded from
// their source URLs via Loader (mirrors GlobalSearchDialogQmlTest) so each
// control's base directory is the qml source dir and its sibling components
// (IconActionButton, AdjustmentResetButton, CollapsibleSection) resolve. The
// models are C++ context properties with the submitter already wired; each
// Loader binds its item's `model` on load.
constexpr char kHarnessQml[] = R"(
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
    id: root
    objectName: "adjustmentControlHarness"
    width: 640
    height: 320
    visible: true
    color: "#111214"

    property var sliderItem: null
    property var toggleItem: null
    property var comboItem: null

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        Loader {
            Layout.fillWidth: true
            Layout.preferredHeight: 56
            source: sliderSourceUrl
            onLoaded: {
                if (item) {
                    item.model = exposureModel
                    item.objectName = "testSlider"
                    root.sliderItem = item
                }
            }
        }
        Loader {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            source: toggleSourceUrl
            onLoaded: {
                if (item) {
                    item.model = toggleModel
                    item.objectName = "testToggle"
                    root.toggleItem = item
                }
            }
        }
        Loader {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            source: comboSourceUrl
            onLoaded: {
                if (item) {
                    item.model = comboModel
                    item.objectName = "testCombo"
                    root.comboItem = item
                }
            }
        }
    }
}
)";

// Per-test setup: fresh submitter + models + engine + loaded harness.
struct Harness {
  RecordingSubmitter             submitter;
  EditorAdjustmentValueModel     exposure;
  EditorAdjustmentToggleModel    toggle;
  EditorAdjustmentEnumModel      combo;
  QQmlApplicationEngine          engine;
  QQuickWindow*                  window = nullptr;
  QStringList                    warnings;

  Harness() {
    QObject::connect(&engine, &QQmlEngine::warnings,
                     [this](const QList<QQmlError>& ws) {
                       for (const auto& w : ws) {
                         warnings << w.toString();
                       }
                     });
    exposure.setFieldKey("exposure");
    exposure.setLabel("Exposure");
    exposure.setMinimum(-5.0);
    exposure.setMaximum(5.0);
    exposure.setDefaultValue(0.0);
    exposure.setStep(0.1);
    exposure.setPrecision(2);
    exposure.setValue(0.0);
    exposure.setSubmitter(&submitter);
    exposure.setDebounceIntervalMs(0);

    toggle.setFieldKey("lens_calib_enabled");
    toggle.setLabel("Lens Calibration");
    toggle.setDefaultValue(false);
    toggle.setValue(false);
    toggle.setSubmitter(&submitter);

    combo.setFieldKey("color_temp_mode");
    combo.setLabel("Color Temp Mode");
    QVariantMap e0;
    e0["value"] = QStringLiteral("as_shot");
    e0["label"] = QStringLiteral("As Shot");
    QVariantMap e1;
    e1["value"] = QStringLiteral("custom");
    e1["label"] = QStringLiteral("Custom");
    QVariantList entries;
    entries << e0 << e1;
    combo.setEntries(entries);
    combo.setDefaultIndex(0);
    combo.setCurrentIndex(0);
    combo.setSubmitter(&submitter);

    AppTheme::Instance().setReduceMotion(true);
    engine.addImportPath(QStringLiteral("qrc:/"));
    engine.addImportPath(SrcQmlDir());
    engine.rootContext()->setContextProperty(QStringLiteral("appTheme"),
                                             &AppTheme::Instance());
    engine.rootContext()->setContextProperty(QStringLiteral("exposureModel"), &exposure);
    engine.rootContext()->setContextProperty(QStringLiteral("toggleModel"), &toggle);
    engine.rootContext()->setContextProperty(QStringLiteral("comboModel"), &combo);
    engine.rootContext()->setContextProperty(QStringLiteral("sliderSourceUrl"), SliderQmlUrl());
    engine.rootContext()->setContextProperty(QStringLiteral("toggleSourceUrl"), ToggleQmlUrl());
    engine.rootContext()->setContextProperty(QStringLiteral("comboSourceUrl"), ComboQmlUrl());
    engine.loadData(QByteArray{kHarnessQml},
                    QUrl(QStringLiteral("file:///AdjustmentControlTestHarness.qml")));
    window = qobject_cast<QQuickWindow*>(engine.rootObjects().value(0, nullptr));
    if (window != nullptr) {
      // Synchronous Loaders finish during creation, but let the scene settle.
      ProcessEvents(100);
    }
  }

  auto find(QString objectName) -> QQuickItem* {
    if (window == nullptr) {
      return nullptr;
    }
    return window->findChild<QQuickItem*>(objectName);
  }
};

auto centerInWindow(QQuickItem* item) -> QPoint {
  QPointF local(item->width() / 2.0, item->height() / 2.0);
  QPointF scene = item->mapToScene(local);
  return scene.toPoint();
}

// Slider keyboard arrow adjusts the value and submits an interactive patch,
// then one settled patch after the debounce fires.
TEST(EditorAdjustmentControlQmlTest, SliderKeyboardArrowAdjustsValueAndSubmitsInteractiveThenSettled) {
  Harness h;
  ASSERT_NE(h.window, nullptr) << "QML harness failed to load. Warnings:\n"
                               << h.warnings.join(QStringLiteral("\n")).toStdString();
  auto* slider = h.find(QStringLiteral("testSlider"));
  ASSERT_NE(slider, nullptr);
  auto* handle = slider->findChild<QQuickItem*>(QStringLiteral("adjustmentSliderHandle"));
  ASSERT_NE(handle, nullptr);
  handle->forceActiveFocus();
  ProcessEvents(50);
  ASSERT_EQ(h.submitter.calls.size(), 0u);
  QTest::keyClick(h.window, Qt::Key_Right);
  // The interval-0 debounce timer fires within this event-loop window.
  ProcessEvents(150);
  ASSERT_GT(h.submitter.interactiveCount(), 0)
      << "slider Keys.onRightPressed did not call model.editValue";
  EXPECT_GT(h.exposure.value(), 0.0);
  EXPECT_EQ(h.submitter.settledForField(QStringLiteral("exposure")), 1);
  EXPECT_FALSE(h.exposure.hasPendingSettled());
}

// Field typing + Enter commits one settled transaction with the typed value.
TEST(EditorAdjustmentControlQmlTest, TextFieldTypingCommitsSettledOnEnter) {
  Harness h;
  ASSERT_NE(h.window, nullptr) << "QML harness failed to load. Warnings:\n"
                               << h.warnings.join(QStringLiteral("\n")).toStdString();
  auto* slider = h.find(QStringLiteral("testSlider"));
  ASSERT_NE(slider, nullptr);
  auto* field = slider->findChild<QQuickItem*>(QStringLiteral("adjustmentSliderField"));
  ASSERT_NE(field, nullptr);
  field->forceActiveFocus();
  ProcessEvents(50);
  TypeText(h.window, QStringLiteral("0.5"));
  QTest::keyClick(h.window, Qt::Key_Return);
  ProcessEvents(50);
  EXPECT_EQ(h.submitter.settledForField(QStringLiteral("exposure")), 1);
  EXPECT_GE(h.submitter.interactiveCount(), 1);
  EXPECT_DOUBLE_EQ(h.exposure.value(), 0.5);
}

// Reset button click restores the default and commits one settled transaction.
TEST(EditorAdjustmentControlQmlTest, ResetButtonClickRestoresDefaultAndSubmitsOneSettled) {
  Harness h;
  ASSERT_NE(h.window, nullptr) << "QML harness failed to load. Warnings:\n"
                               << h.warnings.join(QStringLiteral("\n")).toStdString();
  auto* slider = h.find(QStringLiteral("testSlider"));
  ASSERT_NE(slider, nullptr);
  // Move the value away from default first via the model (programmatic, no submit).
  h.exposure.setValue(0.7);
  ASSERT_EQ(h.submitter.calls.size(), 0u);
  auto* reset = slider->findChild<QQuickItem*>(QStringLiteral("adjustmentResetButton"));
  ASSERT_NE(reset, nullptr);
  QTest::mouseClick(h.window, Qt::LeftButton, {}, centerInWindow(reset));
  ProcessEvents(50);
  EXPECT_EQ(h.submitter.settledForField(QStringLiteral("exposure")), 1);
  EXPECT_DOUBLE_EQ(h.exposure.value(), 0.0);
}

// Invalid field text sets the model invalid and does not submit.
TEST(EditorAdjustmentControlQmlTest, InvalidTextFieldSetsModelInvalidAndDoesNotSubmit) {
  Harness h;
  ASSERT_NE(h.window, nullptr) << "QML harness failed to load. Warnings:\n"
                               << h.warnings.join(QStringLiteral("\n")).toStdString();
  auto* slider = h.find(QStringLiteral("testSlider"));
  ASSERT_NE(slider, nullptr);
  auto* field = slider->findChild<QQuickItem*>(QStringLiteral("adjustmentSliderField"));
  ASSERT_NE(field, nullptr);
  field->forceActiveFocus();
  ProcessEvents(50);
  const auto settledBefore = h.submitter.settledForField(QStringLiteral("exposure"));
  // Drive the field's onEditingFinished handler directly: set non-numeric text
  // and emit editingFinished. (The TextFieldTyping test above covers keyboard
  // entry with valid digits; typing letters via keyClick uses key codes outside
  // Qt::Key's A–Z range, so we exercise the parse-failure path here instead.)
  field->setProperty("text", QStringLiteral("abc"));
  ASSERT_TRUE(QMetaObject::invokeMethod(field, "editingFinished", Qt::DirectConnection));
  ProcessEvents(50);
  EXPECT_FALSE(h.exposure.valid());
  EXPECT_EQ(h.submitter.settledForField(QStringLiteral("exposure")), settledBefore);
}

// A disabled model blocks submission and disables the control.
TEST(EditorAdjustmentControlQmlTest, DisabledModelBlocksSubmitAndDisablesControl) {
  Harness h;
  ASSERT_NE(h.window, nullptr) << "QML harness failed to load. Warnings:\n"
                               << h.warnings.join(QStringLiteral("\n")).toStdString();
  auto* slider = h.find(QStringLiteral("testSlider"));
  ASSERT_NE(slider, nullptr);
  h.exposure.setEnabled(false);
  ProcessEvents(50);
  auto* handle = slider->findChild<QQuickItem*>(QStringLiteral("adjustmentSliderHandle"));
  ASSERT_NE(handle, nullptr);
  EXPECT_FALSE(handle->isEnabled());
  handle->forceActiveFocus();
  QTest::keyClick(h.window, Qt::Key_Right);
  ProcessEvents(50);
  EXPECT_EQ(h.submitter.calls.size(), 0u);
}

// Toggle click commits one settled transaction with the new bool value.
TEST(EditorAdjustmentControlQmlTest, AdjustmentToggleClickSubmitsOneSettled) {
  Harness h;
  ASSERT_NE(h.window, nullptr) << "QML harness failed to load. Warnings:\n"
                               << h.warnings.join(QStringLiteral("\n")).toStdString();
  auto* toggle = h.find(QStringLiteral("testToggle"));
  ASSERT_NE(toggle, nullptr);
  auto* sw = toggle->findChild<QQuickItem*>(QStringLiteral("adjustmentToggleSwitch"));
  ASSERT_NE(sw, nullptr);
  QTest::mouseClick(h.window, Qt::LeftButton, {}, centerInWindow(sw));
  ProcessEvents(50);
  EXPECT_EQ(h.submitter.settledForField(QStringLiteral("lens_calib_enabled")), 1);
  EXPECT_TRUE(h.toggle.value());
  EXPECT_TRUE(RecordingSubmitter::boolValue(
      [&] {
        for (auto it = h.submitter.calls.rbegin(); it != h.submitter.calls.rend(); ++it) {
          if (it->settled && it->fieldKey == QStringLiteral("lens_calib_enabled")) {
            return it->params;
          }
        }
        return QString{};
      }()));
}

// Combo activation commits one settled transaction with the selected index.
TEST(EditorAdjustmentControlQmlTest, AdjustmentComboActivatedSubmitsOneSettled) {
  Harness h;
  ASSERT_NE(h.window, nullptr) << "QML harness failed to load. Warnings:\n"
                               << h.warnings.join(QStringLiteral("\n")).toStdString();
  auto* comboItem = h.find(QStringLiteral("testCombo"));
  ASSERT_NE(comboItem, nullptr);
  auto* combo = comboItem->findChild<QQuickItem*>(QStringLiteral("adjustmentCombo"));
  ASSERT_NE(combo, nullptr);
  // Emit activated(1) to drive the onActivated → selectIndex wiring.
  ASSERT_TRUE(QMetaObject::invokeMethod(combo, "activated", Qt::DirectConnection,
                                        Q_ARG(int, 1)));
  ProcessEvents(50);
  EXPECT_EQ(h.submitter.settledForField(QStringLiteral("color_temp_mode")), 1);
  EXPECT_EQ(h.combo.currentIndex(), 1);
}

}  // namespace
}  // namespace alcedo::ui::test