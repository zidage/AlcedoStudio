//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Phase 6A unit tests for the typed adjustment models. Verifies pointer-drag
// behavior (one settled transaction per completed drag), the
// debounced settled commit for keyboard/wheel bursts, clamping, validation,
// enum/toggle commits, canEdit gating, and the load-vs-edit separation. The
// models are exercised directly in C++ with a recording submitter fake; no QML
// or GPU is involved. QSignalSpy drives the debounce timer deterministically
// through the event loop (interval 0 fires on the next loop iteration).

#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_submitter.hpp"

#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

namespace alcedo::ui::test {
namespace {

// Recording IEditorAdjustmentSubmitter fake. Derives from QObject so the model's
// setSubmitter(QObject*) dynamic_cast resolves it. No Q_OBJECT: it exposes no
// signals/slots/properties.
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
  auto lastSettledParams() const -> QString {
    for (auto it = calls.rbegin(); it != calls.rend(); ++it) {
      if (it->settled) {
        return it->params;
      }
    }
    return {};
  }
  // Parse {"value": v} (numeric/toggle) or {"index": i, "value": s} (enum).
  static auto numericValue(const QString& params) -> double {
    const auto  doc = QJsonDocument::fromJson(params.toUtf8());
    const auto& obj = doc.object();
    if (obj.contains("value") && obj.value("value").isDouble()) {
      return obj.value("value").toDouble();
    }
    return 0.0;
  }
  static auto boolValue(const QString& params) -> bool {
    return QJsonDocument::fromJson(params.toUtf8()).object().value("value").toBool();
  }
  static auto enumIndex(const QString& params) -> int {
    return QJsonDocument::fromJson(params.toUtf8()).object().value("index").toInt();
  }
  static auto enumValue(const QString& params) -> QString {
    return QJsonDocument::fromJson(params.toUtf8()).object().value("value").toString();
  }
};

auto makeValueModel(RecordingSubmitter& sub) -> std::unique_ptr<EditorAdjustmentValueModel> {
  auto m = std::make_unique<EditorAdjustmentValueModel>();
  m->setSubmitter(&sub);
  m->setFieldKey("exposure");
  m->setLabel("Exposure");
  m->setMinimum(-5.0);
  m->setMaximum(5.0);
  m->setDefaultValue(0.0);
  m->setStep(0.1);
  m->setPrecision(2);
  m->setValue(0.0);  // programmatic load: no submit
  sub.calls.clear();
  return m;
}

auto makeEnumModel(RecordingSubmitter& sub) -> std::unique_ptr<EditorAdjustmentEnumModel> {
  auto m = std::make_unique<EditorAdjustmentEnumModel>();
  m->setSubmitter(&sub);
  m->setFieldKey("color_temp_mode");
  m->setLabel("Color Temp Mode");
  QVariantMap e0;
  e0["value"] = QStringLiteral("as_shot");
  e0["label"] = QStringLiteral("As Shot");
  QVariantMap e1;
  e1["value"] = QStringLiteral("custom");
  e1["label"] = QStringLiteral("Custom");
  QVariantList entries;
  entries << e0 << e1;
  m->setEntries(entries);
  m->setDefaultIndex(0);
  m->setCurrentIndex(0);  // programmatic load: no submit
  sub.calls.clear();
  return m;
}

auto makeToggleModel(RecordingSubmitter& sub) -> std::unique_ptr<EditorAdjustmentToggleModel> {
  auto m = std::make_unique<EditorAdjustmentToggleModel>();
  m->setSubmitter(&sub);
  m->setFieldKey("lens_calib_enabled");
  m->setLabel("Lens Calibration");
  m->setDefaultValue(false);
  m->setValue(false);  // programmatic load: no submit
  sub.calls.clear();
  return m;
}

// 1. A pointer drag submits one interactive patch per update and exactly one
// settled patch on release.
TEST(EditorAdjustmentModelTest, PointerDragSubmitsInteractivePerUpdateAndOneSettledOnRelease) {
  RecordingSubmitter sub;
  auto m = makeValueModel(sub);
  m->beginDrag();
  m->updateDrag(0.1);
  m->updateDrag(0.2);
  m->updateDrag(0.3);
  m->finishDrag();
  EXPECT_EQ(sub.interactiveCount(), 3);
  EXPECT_EQ(sub.settledCount(), 1);
  EXPECT_FALSE(m->dragActive());
  EXPECT_DOUBLE_EQ(RecordingSubmitter::numericValue(sub.lastSettledParams()), 0.3);
}

// Control value and submitted live patch are the same object-state at return:
// updateDrag writes value_ then calls submitPatch before returning. There is no
// queued-but-unapplied live value on this path.
TEST(EditorAdjustmentModelTest, PointerDragWritesControlValueAndSubmitsLivePatchBeforeReturn) {
  RecordingSubmitter sub;
  auto               m = makeValueModel(sub);
  m->beginDrag();
  m->updateDrag(0.4);
  ASSERT_EQ(sub.calls.size(), 1u);
  EXPECT_FALSE(sub.calls.front().settled);
  EXPECT_DOUBLE_EQ(m->value(), 0.4);
  EXPECT_DOUBLE_EQ(RecordingSubmitter::numericValue(sub.calls.front().params), m->value());
  m->finishDrag();
}

// 2. A wheel/keyboard burst submits one interactive patch per value and one
// settled patch after the debounce stabilizes.
TEST(EditorAdjustmentModelTest, WheelBurstSubmitsInteractivePerValueAndOneSettledAfterDebounce) {
  RecordingSubmitter sub;
  auto m = makeValueModel(sub);
  m->setDebounceIntervalMs(0);
  m->editValue(0.1);
  m->editValue(0.2);
  m->editValue(0.3);
  EXPECT_TRUE(m->hasPendingSettled());
  EXPECT_EQ(sub.interactiveCount(), 3);
  EXPECT_EQ(sub.settledCount(), 0);
  QSignalSpy spy(m.get(), &EditorAdjustmentValueModel::settledCommitted);
  ASSERT_TRUE(spy.wait(1000));
  EXPECT_EQ(sub.settledCount(), 1);
  EXPECT_FALSE(m->hasPendingSettled());
  EXPECT_DOUBLE_EQ(RecordingSubmitter::numericValue(sub.lastSettledParams()), 0.3);
}

// 3. Keyboard Enter (commitImmediately) forces the debounced settled commit at
// once: one interactive, one settled.
TEST(EditorAdjustmentModelTest, KeyboardEnterCommitImmediatelySubmitsOneSettled) {
  RecordingSubmitter sub;
  auto m = makeValueModel(sub);
  m->setDebounceIntervalMs(0);
  m->editValue(0.5);
  EXPECT_EQ(sub.interactiveCount(), 1);
  EXPECT_TRUE(m->hasPendingSettled());
  m->commitImmediately();
  EXPECT_EQ(sub.settledCount(), 1);
  EXPECT_FALSE(m->hasPendingSettled());
  EXPECT_DOUBLE_EQ(RecordingSubmitter::numericValue(sub.lastSettledParams()), 0.5);
}

// 4. Reset restores the default and commits exactly one settled transaction.
TEST(EditorAdjustmentModelTest, ResetSubmitsOneSettledWithDefaultValue) {
  RecordingSubmitter sub;
  auto m = makeValueModel(sub);
  m->setValue(0.7);  // programmatic load: no submit
  ASSERT_EQ(sub.calls.size(), 0u);
  m->reset();
  EXPECT_EQ(sub.settledCount(), 1);
  EXPECT_EQ(sub.interactiveCount(), 0);
  EXPECT_DOUBLE_EQ(m->value(), 0.0);
  EXPECT_DOUBLE_EQ(RecordingSubmitter::numericValue(sub.lastSettledParams()), 0.0);
}

// 5. An out-of-range value is clamped to [minimum, maximum] before any submit.
TEST(EditorAdjustmentModelTest, OutOfRangeValueIsClampedBeforeSubmit) {
  RecordingSubmitter sub;
  auto m = makeValueModel(sub);
  m->setDebounceIntervalMs(0);
  m->editValue(100.0);  // above maximum 5.0
  EXPECT_DOUBLE_EQ(m->value(), 5.0);
  EXPECT_EQ(sub.interactiveCount(), 1);
  EXPECT_DOUBLE_EQ(RecordingSubmitter::numericValue(sub.calls.back().params), 5.0);
  m->commitImmediately();
  EXPECT_DOUBLE_EQ(RecordingSubmitter::numericValue(sub.lastSettledParams()), 5.0);
}

// 6. A reported invalid field entry sets valid=false and does not submit; a
// subsequent valid edit clears the invalid state and submits.
TEST(EditorAdjustmentModelTest, InvalidValueSetsValidFalseAndDoesNotSubmit) {
  RecordingSubmitter sub;
  auto m = makeValueModel(sub);
  ASSERT_TRUE(m->valid());
  m->setInvalid(QStringLiteral("not a number"));
  EXPECT_FALSE(m->valid());
  EXPECT_EQ(m->errorMessage().toStdString(), "not a number");
  EXPECT_EQ(sub.calls.size(), 0u);
  m->editValue(0.25);
  EXPECT_TRUE(m->valid());
  EXPECT_EQ(sub.interactiveCount(), 1);
  m->commitImmediately();  // drain the debounce so no timer outlives the test
}

// 7. An enum selection commits exactly one settled transaction with the chosen
// index and value.
TEST(EditorAdjustmentModelTest, EnumChangeSubmitsExactlyOneSettledTransaction) {
  RecordingSubmitter sub;
  auto m = makeEnumModel(sub);
  m->selectIndex(1);
  EXPECT_EQ(sub.settledCount(), 1);
  EXPECT_EQ(sub.interactiveCount(), 0);
  EXPECT_EQ(m->currentIndex(), 1);
  EXPECT_EQ(RecordingSubmitter::enumIndex(sub.lastSettledParams()), 1);
  EXPECT_EQ(RecordingSubmitter::enumValue(sub.lastSettledParams()).toStdString(), "custom");
}

// 8. A toggle change commits exactly one settled transaction with the new bool.
TEST(EditorAdjustmentModelTest, ToggleChangeSubmitsExactlyOneSettledTransaction) {
  RecordingSubmitter sub;
  auto m = makeToggleModel(sub);
  m->commitValue(true);
  EXPECT_EQ(sub.settledCount(), 1);
  EXPECT_EQ(sub.interactiveCount(), 0);
  EXPECT_TRUE(m->value());
  EXPECT_TRUE(RecordingSubmitter::boolValue(sub.lastSettledParams()));
}

// 9. When the submitter reports canEdit() false (no image / not Interactive),
// no patch is submitted, but the local value still updates for UI feedback.
TEST(EditorAdjustmentModelTest, NoSubmitWhenSubmitterCanEditFalse) {
  RecordingSubmitter sub;
  sub.canEditState = false;
  auto m = makeValueModel(sub);
  m->beginDrag();
  m->updateDrag(0.5);
  m->finishDrag();
  EXPECT_EQ(sub.calls.size(), 0u);
  EXPECT_DOUBLE_EQ(m->value(), 0.5);
  EXPECT_FALSE(m->dragActive());
}

// 10. The settled commit carries the latest value when multiple edits land
// before the debounce fires.
TEST(EditorAdjustmentModelTest, LatestValueWinsWhenMultipleUpdatesBeforeSettled) {
  RecordingSubmitter sub;
  auto m = makeValueModel(sub);
  m->setDebounceIntervalMs(0);
  m->editValue(0.4);
  m->editValue(0.8);
  QSignalSpy spy(m.get(), &EditorAdjustmentValueModel::settledCommitted);
  ASSERT_TRUE(spy.wait(1000));
  EXPECT_EQ(sub.settledCount(), 1);
  EXPECT_DOUBLE_EQ(RecordingSubmitter::numericValue(sub.lastSettledParams()), 0.8);
  EXPECT_DOUBLE_EQ(m->value(), 0.8);
}

// 11. If the session becomes non-editable during a drag, the settled commit on
// release is dropped silently (documented limitation); the drag still ends.
TEST(EditorAdjustmentModelTest, FinishDragAfterSessionLostDropsSettledSilently) {
  RecordingSubmitter sub;
  auto m = makeValueModel(sub);
  m->beginDrag();
  m->updateDrag(0.5);
  EXPECT_EQ(sub.interactiveCount(), 1);
  sub.canEditState = false;  // session lost during the drag
  m->finishDrag();
  EXPECT_EQ(sub.settledCount(), 0);
  EXPECT_FALSE(m->dragActive());
}

// 12. hasPendingSettled tracks the debounce timer: false at rest, true after an
// editValue, false again after the settled commit lands.
TEST(EditorAdjustmentModelTest, HasPendingSettledTrueAfterValueFalseAfterCommit) {
  RecordingSubmitter sub;
  auto m = makeValueModel(sub);
  m->setDebounceIntervalMs(0);
  EXPECT_FALSE(m->hasPendingSettled());
  m->editValue(0.3);
  EXPECT_TRUE(m->hasPendingSettled());
  m->commitImmediately();
  EXPECT_FALSE(m->hasPendingSettled());
}

}  // namespace
}  // namespace alcedo::ui::test
