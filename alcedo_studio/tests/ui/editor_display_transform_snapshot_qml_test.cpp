//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_display_transform_snapshot_qml_test.cpp
/// @brief Verifies the Display Transform panel loads through
/// EditorAdjustmentStack, models are accessible with correct defaults, and
/// method switching submits through the submitter seam.

#include <gtest/gtest.h>

#include <QApplication>
#include <QCoreApplication>
#include <QEventLoop>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTest>
#include <QVariantMap>
#include <QWheelEvent>
#include <algorithm>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_submitter.hpp"
#include "ui/alcedo_main/app_theme.hpp"

namespace alcedo::ui::test {
namespace {

class DisplayTransformSession final : public QObject, public IEditorAdjustmentSubmitter {
  Q_OBJECT
  Q_PROPERTY(
      QVariantMap adjustmentSnapshot READ adjustmentSnapshot NOTIFY AdjustmentSnapshotChanged)
  Q_PROPERTY(quint64 snapshotRevision READ snapshotRevision NOTIFY AdjustmentSnapshotChanged)
  Q_PROPERTY(QString activeAdjustmentPanel READ activeAdjustmentPanel WRITE setActiveAdjustmentPanel
                 NOTIFY activeAdjustmentPanelChanged)

 public:
  explicit DisplayTransformSession(QVariantMap snapshot, quint64 revision)
      : snapshot_(std::move(snapshot)), revision_(revision) {}

  auto adjustmentSnapshot() const -> QVariantMap { return snapshot_; }
  void setAdjustmentSnapshot(QVariantMap snapshot) {
    snapshot_ = std::move(snapshot);
    ++revision_;
    emit AdjustmentSnapshotChanged();
  }
  auto snapshotRevision() const -> quint64 { return revision_; }
  auto activeAdjustmentPanel() const -> QString { return active_panel_; }
  void setActiveAdjustmentPanel(const QString& panel) {
    if (active_panel_ != panel) {
      active_panel_ = panel;
      emit activeAdjustmentPanelChanged();
    }
  }
  auto submitWrite(QString fieldKey, alcedo::EditorParameterWrite write, bool settled)
      -> bool override {
    static_cast<void>(fieldKey);
    static_cast<void>(settled);
    writes_.push_back(std::move(write));
    ++submit_count_;
    return true;
  }

  auto submitPatch(QString fieldKey, QString paramsJson, bool settled) -> bool override {
    nlohmann::json parsed;
    try {
      parsed = paramsJson.isEmpty() ? nlohmann::json::object()
                                    : nlohmann::json::parse(paramsJson.toStdString());
    } catch (const std::exception&) {
      return false;
    }
    std::string error;
    auto        write = alcedo::ParseEditorParameterWrite(fieldKey.toStdString(), parsed, &error);
    if (!write.has_value()) {
      return false;
    }
    return submitWrite(std::move(fieldKey), std::move(*write), settled);
  }
  auto canEdit() const -> bool override { return true; }
  auto submitCount() const -> int { return submit_count_; }
  auto writes() const -> const std::vector<alcedo::EditorParameterWrite>& { return writes_; }

 signals:
  void AdjustmentSnapshotChanged();
  void activeAdjustmentPanelChanged();

 private:
  QVariantMap                               snapshot_;
  quint64                                   revision_     = 0;
  int                                       submit_count_ = 0;
  QString                                   active_panel_ = QStringLiteral("display");
  std::vector<alcedo::EditorParameterWrite> writes_;
};

auto QmlDirectory() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

auto AdjustmentStackUrl() -> QUrl {
  return QUrl::fromLocalFile(QmlDirectory() + QStringLiteral("/EditorAdjustmentStack.qml"));
}

void RegisterQmlTypesOnce() {
  static std::once_flag once;
  std::call_once(once, [] { RegisterEditorAdjustmentQmlTypes(); });
}

class AdjustmentStackHarness {
 public:
  explicit AdjustmentStackHarness(DisplayTransformSession* session) {
    RegisterQmlTypesOnce();
    AppTheme::RegisterFonts();
    AppTheme::Instance().setReduceMotion(true);
    QQuickStyle::setStyle(QStringLiteral("Material"));

    engine_.addImportPath(QStringLiteral("qrc:/"));
    engine_.addImportPath(QmlDirectory());
    engine_.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());

    QQmlComponent component(&engine_, AdjustmentStackUrl());
    if (component.isError()) {
      errors_ = component.errors();
      return;
    }

    QVariantMap initial_properties;
    initial_properties.insert(QStringLiteral("editorSession"),
                              QVariant::fromValue(static_cast<QObject*>(session)));
    initial_properties.insert(QStringLiteral("controlsEnabled"), true);
    root_.reset(
        qobject_cast<QQuickItem*>(component.createWithInitialProperties(initial_properties)));
    if (!root_) {
      errors_ = component.errors();
      return;
    }

    root_->setParentItem(window_.contentItem());
    window_.resize(400, 700);
    root_->setSize(QSizeF(400, 700));
    window_.show();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
  }

  auto window() -> QQuickWindow* { return &window_; }

  auto root() const -> QQuickItem* { return root_.get(); }
  auto errors() const -> QString {
    QStringList text;
    for (const auto& error : errors_) {
      text.push_back(error.toString());
    }
    return text.join('\n');
  }

  template <typename T>
  T* findModel(const QString& objectName) const {
    return root_ ? root_->findChild<T*>(objectName) : nullptr;
  }

 private:
  QQmlEngine                  engine_;
  QQuickWindow                window_;
  std::unique_ptr<QQuickItem> root_;
  QList<QQmlError>            errors_;
};

auto MakeOpenDrtSnapshot(double         peak           = 100.0,
                         const QString& encoding_space = QStringLiteral("rec709"),
                         const QString& encoding_eotf  = QStringLiteral("gamma_2_2"),
                         const QString& look_preset    = QStringLiteral("standard"),
                         const QString& tonescale      = QStringLiteral("use_look_preset"),
                         const QString& creative_white = QStringLiteral("use_look_preset"))
    -> QVariantMap {
  QVariantMap inner;
  inner.insert(QStringLiteral("method"), QStringLiteral("open_drt"));
  inner.insert(QStringLiteral("encoding_space"), encoding_space);
  inner.insert(QStringLiteral("encoding_eotf"), encoding_eotf);
  inner.insert(QStringLiteral("peak_luminance"), peak);

  QVariantMap open_drt;
  open_drt.insert(QStringLiteral("look_preset"), look_preset);
  open_drt.insert(QStringLiteral("tonescale_preset"), tonescale);
  open_drt.insert(QStringLiteral("creative_white"), creative_white);
  inner.insert(QStringLiteral("open_drt"), open_drt);

  QVariantMap odt_wrap;
  odt_wrap.insert(QStringLiteral("odt"), inner);

  QVariantMap snapshot;
  snapshot.insert(QStringLiteral("odt"), odt_wrap);
  return snapshot;
}

/// The projection form of a film print document, as PanelProjectionToVariantMap writes it.
auto MakeFilmPrintSnapshot(double         halation = 2.5,
                           const QString& negative = QStringLiteral("kodak_portra_400"))
    -> QVariantMap {
  QVariantMap film_print;
  film_print.insert(QStringLiteral("negative"), negative);
  film_print.insert(QStringLiteral("print_material"), QStringLiteral("fuji_ca_maxima"));
  film_print.insert(QStringLiteral("scene_light"), QStringLiteral("tungsten"));
  film_print.insert(QStringLiteral("light_spread"), 0.5);
  film_print.insert(QStringLiteral("halation"), halation);
  film_print.insert(QStringLiteral("enlarger_flare"), 0.05);

  auto snapshot = MakeOpenDrtSnapshot();
  auto odt_wrap = snapshot.value(QStringLiteral("odt")).toMap();
  auto inner    = odt_wrap.value(QStringLiteral("odt")).toMap();
  inner.insert(QStringLiteral("method"), QStringLiteral("film_print"));
  inner.insert(QStringLiteral("film_print"), film_print);
  odt_wrap.insert(QStringLiteral("odt"), inner);
  snapshot.insert(QStringLiteral("odt"), odt_wrap);
  return snapshot;
}

/// Values of the browser entries of a film print picker.
auto PickerValues(QObject* picker) -> std::vector<std::string> {
  std::vector<std::string> values;
  for (const auto& entry : picker->property("entries").toList()) {
    values.push_back(entry.toMap().value(QStringLiteral("value")).toString().toStdString());
  }
  return values;
}

/// Values of the entry rows that a film print picker shows (after the search and brand filter).
auto PickerShownValues(QObject* picker) -> std::vector<std::string> {
  std::vector<std::string> values;
  for (const auto& row : picker->property("rows").toList()) {
    const auto map = row.toMap();
    if (map.value(QStringLiteral("kind")).toString() == QStringLiteral("entry")) {
      values.push_back(map.value(QStringLiteral("entry"))
                           .toMap()
                           .value(QStringLiteral("value"))
                           .toString()
                           .toStdString());
    }
  }
  return values;
}

/// The last film print write of the session, or null.
auto LastDrtWrite(const DisplayTransformSession& session) -> const alcedo::DrtParameterUpdate* {
  if (session.writes().empty()) {
    return nullptr;
  }
  return std::get_if<alcedo::DrtParameterUpdate>(&session.writes().back());
}

template <typename Predicate>
auto WaitFor(Predicate predicate) -> bool {
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (predicate()) {
      return true;
    }
    QTest::qWait(10);
  }
  return predicate();
}

}  // namespace

// Panel loads via EditorAdjustmentStack and models are accessible.

TEST(EditorDisplayTransformSnapshotQmlTest, PanelLoadsAndAllModelsAreAccessible) {
  auto                    snapshot = MakeOpenDrtSnapshot();
  DisplayTransformSession session(snapshot, 0);
  AdjustmentStackHarness  harness(&session);

  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  // All 8 display transform models must be found
  ASSERT_NE(harness.findModel<QObject>(QStringLiteral("displayMethodModel")), nullptr);
  ASSERT_NE(harness.findModel<QObject>(QStringLiteral("displayEncodingSpaceModel")), nullptr);
  ASSERT_NE(harness.findModel<QObject>(QStringLiteral("displayEncodingEotfModel")), nullptr);
  ASSERT_NE(harness.findModel<QObject>(QStringLiteral("displayPeakLuminanceModel")), nullptr);
  ASSERT_NE(harness.findModel<QObject>(QStringLiteral("displayAcesLimitingSpaceModel")), nullptr);
  ASSERT_NE(harness.findModel<QObject>(QStringLiteral("displayOpenDrtLookModel")), nullptr);
  ASSERT_NE(harness.findModel<QObject>(QStringLiteral("displayOpenDrtTonescaleModel")), nullptr);
  ASSERT_NE(harness.findModel<QObject>(QStringLiteral("displayOpenDrtCreativeWhiteModel")),
            nullptr);
}

TEST(EditorDisplayTransformSnapshotQmlTest, MethodModelDefaultIsOpenDrt) {
  auto                    snapshot = MakeOpenDrtSnapshot();
  DisplayTransformSession session(snapshot, 0);
  AdjustmentStackHarness  harness(&session);

  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* methodModel = harness.findModel<QObject>(QStringLiteral("displayMethodModel"));
  ASSERT_NE(methodModel, nullptr);

  // defaultIndex is 1 (open_drt)
  EXPECT_EQ(methodModel->property("defaultIndex").toInt(), 1);

  // Entries in the order of the catalog.
  auto entries = methodModel->property("entries").toList();
  ASSERT_EQ(entries.size(), 3);
  EXPECT_EQ(entries[0].toMap().value("value").toString().toStdString(), "aces_2_0");
  EXPECT_EQ(entries[1].toMap().value("value").toString().toStdString(), "open_drt");
  EXPECT_EQ(entries[2].toMap().value("value").toString().toStdString(), "film_print");
}

TEST(EditorDisplayTransformSnapshotQmlTest, EncodingSpaceListsTheModelSpaces) {
  auto                    snapshot = MakeOpenDrtSnapshot();
  DisplayTransformSession session(snapshot, 0);
  AdjustmentStackHarness  harness(&session);

  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* spaceModel = harness.findModel<QObject>(QStringLiteral("displayEncodingSpaceModel"));
  ASSERT_NE(spaceModel, nullptr);

  // The DrtColorSpace values of the display transform Model, from the catalog.
  auto entries = spaceModel->property("entries").toList();
  ASSERT_EQ(entries.size(), 3);
  EXPECT_EQ(entries[0].toMap().value("value").toString().toStdString(), "rec709");
  EXPECT_EQ(entries[1].toMap().value("value").toString().toStdString(), "p3_d65");
  EXPECT_EQ(entries[2].toMap().value("value").toString().toStdString(), "rec2020");
}

TEST(EditorDisplayTransformSnapshotQmlTest, EncodingSpaceChangeSelectsAnEotfOfTheSpace) {
  auto                    snapshot = MakeOpenDrtSnapshot();
  DisplayTransformSession session(snapshot, 0);
  AdjustmentStackHarness  harness(&session);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* space_model =
      harness.findModel<EditorAdjustmentEnumModel>(QStringLiteral("displayEncodingSpaceModel"));
  auto* eotf_model = harness.findModel<QObject>(QStringLiteral("displayEncodingEotfModel"));
  ASSERT_NE(space_model, nullptr);
  ASSERT_NE(eotf_model, nullptr);

  space_model->selectIndex(2);  // rec2020
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

  const auto entries = eotf_model->property("entries").toList();
  ASSERT_EQ(entries.size(), 2);
  EXPECT_EQ(entries[0].toMap().value("value").toString().toStdString(), "st2084");
  EXPECT_EQ(entries[1].toMap().value("value").toString().toStdString(), "hlg");
  EXPECT_EQ(eotf_model->property("currentValue").toString().toStdString(), "st2084");

  ASSERT_GE(session.submitCount(), 1);
  const auto* drt = std::get_if<alcedo::DrtParameterUpdate>(&session.writes().back());
  ASSERT_NE(drt, nullptr);
  ASSERT_TRUE(drt->encoding_space.has_value());
  EXPECT_EQ(*drt->encoding_space, alcedo::DrtColorSpace::Rec2020);
  ASSERT_TRUE(drt->encoding_eotf.has_value());
  EXPECT_EQ(*drt->encoding_eotf, alcedo::DrtEotf::St2084);
}

TEST(EditorDisplayTransformSnapshotQmlTest, PeakLuminanceRangeIsCorrect) {
  auto                    snapshot = MakeOpenDrtSnapshot();
  DisplayTransformSession session(snapshot, 0);
  AdjustmentStackHarness  harness(&session);

  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* peakModel = harness.findModel<QObject>(QStringLiteral("displayPeakLuminanceModel"));
  ASSERT_NE(peakModel, nullptr);

  EXPECT_DOUBLE_EQ(peakModel->property("minimum").toDouble(), 100.0);
  EXPECT_DOUBLE_EQ(peakModel->property("maximum").toDouble(), 1000.0);
  EXPECT_DOUBLE_EQ(peakModel->property("defaultValue").toDouble(), 100.0);
  EXPECT_EQ(peakModel->property("suffix").toString().toStdString(), " nits");
}

TEST(EditorDisplayTransformSnapshotQmlTest, SubmitterIsWiredOnModel) {
  auto                    snapshot = MakeOpenDrtSnapshot();
  DisplayTransformSession session(snapshot, 0);
  AdjustmentStackHarness  harness(&session);

  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* methodModel = harness.findModel<QObject>(QStringLiteral("displayMethodModel"));
  ASSERT_NE(methodModel, nullptr);

  auto* submitterObj = methodModel->property("submitter").value<QObject*>();
  ASSERT_NE(submitterObj, nullptr) << "submitter property not set on model";
  EXPECT_EQ(submitterObj, static_cast<QObject*>(&session));
}

TEST(EditorDisplayTransformSnapshotQmlTest, OpenDrtModelEntriesAreCorrect) {
  auto                    snapshot = MakeOpenDrtSnapshot();
  DisplayTransformSession session(snapshot, 0);
  AdjustmentStackHarness  harness(&session);

  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* lookModel = harness.findModel<QObject>(QStringLiteral("displayOpenDrtLookModel"));
  ASSERT_NE(lookModel, nullptr);

  auto entries = lookModel->property("entries").toList();
  ASSERT_EQ(entries.size(), 8);
  EXPECT_EQ(entries[0].toMap().value("value").toString().toStdString(), "standard");
  EXPECT_EQ(entries[1].toMap().value("value").toString().toStdString(), "arriba");
}

TEST(EditorDisplayTransformSnapshotQmlTest, SubmitterFieldKeyIsOdt) {
  auto                    snapshot = MakeOpenDrtSnapshot();
  DisplayTransformSession session(snapshot, 0);
  AdjustmentStackHarness  harness(&session);

  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* spaceModel = harness.findModel<QObject>(QStringLiteral("displayEncodingSpaceModel"));
  ASSERT_NE(spaceModel, nullptr);

  EXPECT_EQ(spaceModel->property("fieldKey").toString().toStdString(), "odt");
}

// Non-default ODT snapshot must project into every display model (load-only).
TEST(EditorDisplayTransformSnapshotQmlTest, LoadFromSnapshotProjectsNonDefaultOdtFields) {
  auto snapshot = MakeOpenDrtSnapshot(
      /*peak=*/420.0, QStringLiteral("rec2020"), QStringLiteral("st2084"), QStringLiteral("umbra"),
      QStringLiteral("aces_2_0"), QStringLiteral("d60"));
  DisplayTransformSession session(snapshot, 7);
  AdjustmentStackHarness  harness(&session);

  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* methodModel = harness.findModel<QObject>(QStringLiteral("displayMethodModel"));
  auto* spaceModel  = harness.findModel<QObject>(QStringLiteral("displayEncodingSpaceModel"));
  auto* eotfModel   = harness.findModel<QObject>(QStringLiteral("displayEncodingEotfModel"));
  auto* peakModel   = harness.findModel<QObject>(QStringLiteral("displayPeakLuminanceModel"));
  auto* lookModel   = harness.findModel<QObject>(QStringLiteral("displayOpenDrtLookModel"));
  auto* toneModel   = harness.findModel<QObject>(QStringLiteral("displayOpenDrtTonescaleModel"));
  auto* whiteModel = harness.findModel<QObject>(QStringLiteral("displayOpenDrtCreativeWhiteModel"));
  ASSERT_NE(methodModel, nullptr);
  ASSERT_NE(spaceModel, nullptr);
  ASSERT_NE(eotfModel, nullptr);
  ASSERT_NE(peakModel, nullptr);
  ASSERT_NE(lookModel, nullptr);
  ASSERT_NE(toneModel, nullptr);
  ASSERT_NE(whiteModel, nullptr);

  EXPECT_EQ(methodModel->property("currentValue").toString().toStdString(), "open_drt");
  EXPECT_EQ(spaceModel->property("currentValue").toString().toStdString(), "rec2020");
  EXPECT_EQ(eotfModel->property("currentValue").toString().toStdString(), "st2084");
  EXPECT_DOUBLE_EQ(peakModel->property("value").toDouble(), 420.0);
  EXPECT_EQ(lookModel->property("currentValue").toString().toStdString(), "umbra");
  EXPECT_EQ(toneModel->property("currentValue").toString().toStdString(), "aces_2_0");
  EXPECT_EQ(whiteModel->property("currentValue").toString().toStdString(), "d60");
}

TEST(EditorDisplayTransformSnapshotQmlTest, LoadFromSnapshotProjectsAcesMethodAndLimitingSpace) {
  QVariantMap inner;
  inner.insert(QStringLiteral("method"), QStringLiteral("aces_2_0"));
  inner.insert(QStringLiteral("encoding_space"), QStringLiteral("p3_d65"));
  inner.insert(QStringLiteral("encoding_eotf"), QStringLiteral("gamma_2_2"));
  inner.insert(QStringLiteral("peak_luminance"), 100.0);
  inner.insert(QStringLiteral("limiting_space"), QStringLiteral("rec2020"));

  QVariantMap odt_wrap;
  odt_wrap.insert(QStringLiteral("odt"), inner);
  QVariantMap snapshot;
  snapshot.insert(QStringLiteral("odt"), odt_wrap);

  DisplayTransformSession session(snapshot, 3);
  AdjustmentStackHarness  harness(&session);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* methodModel = harness.findModel<QObject>(QStringLiteral("displayMethodModel"));
  auto* spaceModel  = harness.findModel<QObject>(QStringLiteral("displayEncodingSpaceModel"));
  auto* limitModel  = harness.findModel<QObject>(QStringLiteral("displayAcesLimitingSpaceModel"));
  ASSERT_NE(methodModel, nullptr);
  ASSERT_NE(spaceModel, nullptr);
  ASSERT_NE(limitModel, nullptr);

  EXPECT_EQ(methodModel->property("currentValue").toString().toStdString(), "aces_2_0");
  EXPECT_EQ(methodModel->property("currentIndex").toInt(), 0);
  EXPECT_EQ(spaceModel->property("currentValue").toString().toStdString(), "p3_d65");
  EXPECT_EQ(limitModel->property("currentValue").toString().toStdString(), "rec2020");
}

TEST(EditorDisplayTransformSnapshotQmlTest, DeclaredDefaultsPreferOpenDrtWhenSnapshotMissingOdt) {
  DisplayTransformSession session(QVariantMap{}, 0);
  AdjustmentStackHarness  harness(&session);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* methodModel = harness.findModel<QObject>(QStringLiteral("displayMethodModel"));
  auto* eotfModel   = harness.findModel<QObject>(QStringLiteral("displayEncodingEotfModel"));
  auto* peakModel   = harness.findModel<QObject>(QStringLiteral("displayPeakLuminanceModel"));
  ASSERT_NE(methodModel, nullptr);
  ASSERT_NE(eotfModel, nullptr);
  ASSERT_NE(peakModel, nullptr);

  // Without an odt patch, the panel must still land on declared product defaults
  // (open_drt / gamma_2_2 / 100 nits), not construction zeros (index 0 / value 0).
  EXPECT_EQ(methodModel->property("currentValue").toString().toStdString(), "open_drt");
  EXPECT_EQ(methodModel->property("currentIndex").toInt(), 1);
  EXPECT_EQ(eotfModel->property("currentValue").toString().toStdString(), "gamma_2_2");
  EXPECT_DOUBLE_EQ(peakModel->property("value").toDouble(), 100.0);
}

TEST(EditorDisplayTransformSnapshotQmlTest, MethodChangeEnqueuesDrtParameterUpdate) {
  auto                    snapshot = MakeOpenDrtSnapshot();
  DisplayTransformSession session(snapshot, 0);
  AdjustmentStackHarness  harness(&session);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* method_model =
      harness.findModel<EditorAdjustmentEnumModel>(QStringLiteral("displayMethodModel"));
  ASSERT_NE(method_model, nullptr);
  EXPECT_EQ(session.submitCount(), 0);

  method_model->selectIndex(0);
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

  ASSERT_GE(session.submitCount(), 1);
  const auto* drt = std::get_if<alcedo::DrtParameterUpdate>(&session.writes().back());
  ASSERT_NE(drt, nullptr);
  ASSERT_TRUE(drt->method.has_value());
  EXPECT_EQ(*drt->method, alcedo::DrtMethod::Aces20);
}

TEST(EditorDisplayTransformSnapshotQmlTest,
     WheelAndTouchpadScrollReachTheLastControlInBothMethods) {
  DisplayTransformSession session(MakeOpenDrtSnapshot(), 0);
  AdjustmentStackHarness  harness(&session);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  harness.window()->resize(400, 560);
  harness.root()->setSize(QSizeF(400, 560));
  ASSERT_TRUE(QTest::qWaitForWindowExposed(harness.window()));
  auto* scroll = harness.findModel<QQuickItem>(QStringLiteral("editorDisplayTransformPanelScroll"));
  auto* last   = harness.findModel<QQuickItem>(QStringLiteral("displayPeakLuminanceSlider"));
  auto* method = harness.findModel<EditorAdjustmentEnumModel>(QStringLiteral("displayMethodModel"));
  ASSERT_NE(scroll, nullptr);
  ASSERT_NE(last, nullptr);
  ASSERT_NE(method, nullptr);
  for (const int index : {1, 0}) {
    SCOPED_TRACE(index == 1 ? "OpenDRT" : "ACES");
    method->selectIndex(index);
    scroll->setProperty("contentY", 0.0);
    QTest::qWait(100);
    ASSERT_GT(scroll->height(), 0.0);
    ASSERT_GT(scroll->property("contentHeight").toReal(), scroll->height());
    EXPECT_GT(last->mapToItem(scroll, QPointF(0, last->height())).y(), scroll->height());
    const auto position = scroll->mapToScene(QPointF(scroll->width() - 2, scroll->height() / 2));
    const auto global   = harness.window()->mapToGlobal(position.toPoint());
    auto       wheel    = [&](QPoint pixels, QPoint angle) {
      QWheelEvent event(position, global, pixels, angle, Qt::NoButton, Qt::NoModifier,
                                 Qt::NoScrollPhase, false);
      QCoreApplication::sendEvent(harness.window(), &event);
      QTest::qWait(30);
    };
    wheel({}, QPoint(0, -120));
    EXPECT_GT(scroll->property("contentY").toReal(), 0.0);
    scroll->setProperty("contentY", 0.0);
    wheel(QPoint(0, -35), {});
    EXPECT_GT(scroll->property("contentY").toReal(), 0.0);
    for (int step = 0; step < 30; ++step) wheel({}, QPoint(0, -120));
    const auto max_y = scroll->property("contentHeight").toReal() - scroll->height();
    EXPECT_NEAR(scroll->property("contentY").toReal(), max_y, 1.0);
    EXPECT_GE(last->mapToItem(scroll, QPointF()).y(), -1.0);
    EXPECT_LE(last->mapToItem(scroll, QPointF(0, last->height())).y(), scroll->height() + 1.0);
    for (int step = 0; step < 30; ++step) wheel({}, QPoint(0, 120));
    EXPECT_NEAR(scroll->property("contentY").toReal(), 0.0, 1.0);
  }
}

TEST(EditorDisplayTransformSnapshotQmlTest,
     FilmPrintSnapshotLoadsItsFieldsAndShowsOnlyItsControls) {
  DisplayTransformSession session(MakeOpenDrtSnapshot(), 0);
  AdjustmentStackHarness  harness(&session);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* film_controls = harness.findModel<QQuickItem>(QStringLiteral("displayFilmPrintControls"));
  auto* encoding      = harness.findModel<QQuickItem>(
      QStringLiteral("editorAdjustmentGroupShell_display_color_encoding"));
  auto* look_combo = harness.findModel<QQuickItem>(QStringLiteral("displayOpenDrtLookCombo"));
  auto* limit_combo =
      harness.findModel<QQuickItem>(QStringLiteral("displayAcesLimitingSpaceCombo"));
  ASSERT_NE(film_controls, nullptr);
  ASSERT_NE(encoding, nullptr);
  ASSERT_NE(look_combo, nullptr);
  ASSERT_NE(limit_combo, nullptr);
  ASSERT_TRUE(WaitFor([&] { return encoding->isVisible(); }));
  EXPECT_FALSE(film_controls->isVisible());
  EXPECT_TRUE(look_combo->isVisible());

  session.setAdjustmentSnapshot(MakeFilmPrintSnapshot());
  ASSERT_TRUE(WaitFor([&] { return film_controls->isVisible(); }));
  EXPECT_FALSE(encoding->isVisible());
  EXPECT_FALSE(look_combo->isVisible());
  EXPECT_FALSE(limit_combo->isVisible());

  const auto current = [&](const QString& name) {
    return harness.findModel<QObject>(name)->property("currentValue").toString().toStdString();
  };
  const auto value = [&](const QString& name) {
    return harness.findModel<QObject>(name)->property("value").toDouble();
  };
  EXPECT_EQ(current(QStringLiteral("displayFilmNegativeModel")), "kodak_portra_400");
  EXPECT_EQ(current(QStringLiteral("displayFilmPrintMaterialModel")), "fuji_ca_maxima");
  EXPECT_EQ(current(QStringLiteral("displayFilmSceneLightModel")), "tungsten");
  EXPECT_DOUBLE_EQ(value(QStringLiteral("displayFilmLightSpreadModel")), 0.5);
  EXPECT_DOUBLE_EQ(value(QStringLiteral("displayFilmHalationModel")), 2.5);
  EXPECT_DOUBLE_EQ(value(QStringLiteral("displayFilmEnlargerFlareModel")), 0.05);

  session.setAdjustmentSnapshot(MakeOpenDrtSnapshot());
  ASSERT_TRUE(WaitFor([&] { return encoding->isVisible(); }));
  EXPECT_FALSE(film_controls->isVisible());
}

TEST(EditorDisplayTransformSnapshotQmlTest, SelectingFilmPrintWritesRec709SdrOverAnHdrDocument) {
  DisplayTransformSession session(
      MakeOpenDrtSnapshot(1000.0, QStringLiteral("rec2020"), QStringLiteral("st2084")), 0);
  AdjustmentStackHarness harness(&session);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto* method = harness.findModel<EditorAdjustmentEnumModel>(QStringLiteral("displayMethodModel"));
  ASSERT_NE(method, nullptr);
  ASSERT_TRUE(WaitFor([&] {
    return harness.findModel<QObject>(QStringLiteral("displayEncodingSpaceModel"))
               ->property("currentValue")
               .toString() == QStringLiteral("rec2020");
  }));

  method->selectIndex(2);  // film_print
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

  ASSERT_GE(session.submitCount(), 1);
  const auto* drt = std::get_if<alcedo::DrtParameterUpdate>(&session.writes().back());
  ASSERT_NE(drt, nullptr);
  ASSERT_TRUE(drt->method.has_value());
  EXPECT_EQ(*drt->method, alcedo::DrtMethod::FilmPrint);
  ASSERT_TRUE(drt->encoding_space.has_value());
  EXPECT_EQ(*drt->encoding_space, alcedo::DrtColorSpace::Rec709);
  ASSERT_TRUE(drt->encoding_eotf.has_value());
  EXPECT_EQ(*drt->encoding_eotf, alcedo::DrtEotf::Gamma22);
  ASSERT_TRUE(drt->film_negative.has_value());
  EXPECT_EQ(*drt->film_negative, "kodak_5207");
  ASSERT_TRUE(drt->film_enlarger_flare.has_value());
  EXPECT_FLOAT_EQ(*drt->film_enlarger_flare, 0.01f);
  EXPECT_FALSE(drt->look_preset.has_value());
}

TEST(EditorDisplayTransformSnapshotQmlTest, FilmPrintControlWritesItsField) {
  DisplayTransformSession session(MakeFilmPrintSnapshot(), 0);
  AdjustmentStackHarness  harness(&session);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto* negative =
      harness.findModel<EditorAdjustmentEnumModel>(QStringLiteral("displayFilmNegativeModel"));
  auto* halation =
      harness.findModel<EditorAdjustmentValueModel>(QStringLiteral("displayFilmHalationModel"));
  ASSERT_NE(negative, nullptr);
  ASSERT_NE(halation, nullptr);
  ASSERT_TRUE(WaitFor([&] {
    return negative->property("currentValue").toString() == QStringLiteral("kodak_portra_400");
  }));

  // The Film list is in lab order (lab decision U83), so the test finds the index of the film.
  const auto entries = negative->property("entries").toList();
  int        agfa    = -1;
  for (int index = 0; index < entries.size(); ++index) {
    if (entries[index].toMap().value("value").toString() == QStringLiteral("agfa_vista_100")) {
      agfa = index;
    }
  }
  ASSERT_GE(agfa, 0);
  negative->selectIndex(agfa);
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  ASSERT_GE(session.submitCount(), 1);
  const auto* drt = std::get_if<alcedo::DrtParameterUpdate>(&session.writes().back());
  ASSERT_NE(drt, nullptr);
  ASSERT_TRUE(drt->film_negative.has_value());
  EXPECT_EQ(*drt->film_negative, "agfa_vista_100");
  ASSERT_TRUE(drt->film_halation.has_value());
  EXPECT_FLOAT_EQ(*drt->film_halation, 2.5f);
  ASSERT_TRUE(drt->film_scene_light.has_value());
  EXPECT_EQ(*drt->film_scene_light, "tungsten");
}

TEST(EditorDisplayTransformSnapshotQmlTest,
     ReversalFilmHidesPrintMaterialKeepsItsValueAndNamesTheLoupeFlare) {
  DisplayTransformSession session(MakeFilmPrintSnapshot(), 0);
  AdjustmentStackHarness  harness(&session);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto* material_combo =
      harness.findModel<QQuickItem>(QStringLiteral("displayFilmPrintMaterialPicker"));
  auto* material = harness.findModel<QObject>(QStringLiteral("displayFilmPrintMaterialModel"));
  auto* flare    = harness.findModel<EditorAdjustmentValueModel>(
      QStringLiteral("displayFilmEnlargerFlareModel"));
  auto* negative =
      harness.findModel<EditorAdjustmentEnumModel>(QStringLiteral("displayFilmNegativeModel"));
  ASSERT_NE(material_combo, nullptr);
  ASSERT_NE(material, nullptr);
  ASSERT_NE(flare, nullptr);
  ASSERT_NE(negative, nullptr);
  ASSERT_TRUE(WaitFor([&] { return material_combo->isVisible(); }));
  EXPECT_EQ(flare->property("label").toString().toStdString(), "Enlarger Flare");

  // The Film list has the reversal films after the negatives.
  const auto entries = negative->property("entries").toList();
  ASSERT_GE(entries.size(), 35);
  EXPECT_EQ(entries[30].toMap().value("value").toString().toStdString(), "fuji_velvia_50");
  EXPECT_EQ(entries[34].toMap().value("value").toString().toStdString(), "kodak_ektachrome_e100");

  session.setAdjustmentSnapshot(MakeFilmPrintSnapshot(2.5, QStringLiteral("fuji_velvia_50")));
  ASSERT_TRUE(WaitFor([&] { return !material_combo->isVisible(); }));
  EXPECT_EQ(flare->property("label").toString().toStdString(), "Loupe Flare");
  EXPECT_EQ(material->property("currentValue").toString().toStdString(), "fuji_ca_maxima");

  // A write of the loupe flare keeps the stored print material.
  flare->editValue(0.03);
  flare->commitImmediately();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  ASSERT_GE(session.submitCount(), 1);
  const auto* drt = std::get_if<alcedo::DrtParameterUpdate>(&session.writes().back());
  ASSERT_NE(drt, nullptr);
  ASSERT_TRUE(drt->film_negative.has_value());
  EXPECT_EQ(*drt->film_negative, "fuji_velvia_50");
  ASSERT_TRUE(drt->film_print_material.has_value());
  EXPECT_EQ(*drt->film_print_material, "fuji_ca_maxima");

  session.setAdjustmentSnapshot(MakeFilmPrintSnapshot());
  ASSERT_TRUE(WaitFor([&] { return material_combo->isVisible(); }));
  EXPECT_EQ(flare->property("label").toString().toStdString(), "Enlarger Flare");
}

TEST(EditorDisplayTransformSnapshotQmlTest,
     ProcessSwitchWritesOnlyTheFilmAndReturnsToTheLastFilmOfTheProcess) {
  DisplayTransformSession session(MakeFilmPrintSnapshot(), 0);
  AdjustmentStackHarness  harness(&session);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto* process = harness.findModel<QQuickItem>(QStringLiteral("displayFilmProcessTrack"));
  auto* film    = harness.findModel<QQuickItem>(QStringLiteral("displayFilmNegativePicker"));
  auto* print   = harness.findModel<QQuickItem>(QStringLiteral("displayFilmPrintMaterialPicker"));
  ASSERT_NE(process, nullptr);
  ASSERT_NE(film, nullptr);
  ASSERT_NE(print, nullptr);
  ASSERT_TRUE(WaitFor([&] {
    return film->property("currentValue").toString() == QStringLiteral("kodak_portra_400");
  }));
  EXPECT_EQ(process->property("currentValue").toString().toStdString(), "negative");
  EXPECT_TRUE(print->isVisible());

  // Reversal: no reversal film was used in this session, so the default film is selected.
  ASSERT_TRUE(QMetaObject::invokeMethod(process, "selected", Q_ARG(int, 1),
                                        Q_ARG(QString, QStringLiteral("reversal"))));
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  ASSERT_EQ(session.submitCount(), 1);
  const auto* drt = LastDrtWrite(session);
  ASSERT_NE(drt, nullptr);
  ASSERT_TRUE(drt->film_negative.has_value());
  EXPECT_EQ(*drt->film_negative, "fuji_provia_100f");
  ASSERT_TRUE(drt->film_print_material.has_value());
  EXPECT_EQ(*drt->film_print_material, "fuji_ca_maxima");
  ASSERT_TRUE(WaitFor([&] { return !print->isVisible(); }));
  EXPECT_EQ(process->property("currentValue").toString().toStdString(), "reversal");

  // Negative again: the last negative of the session comes back.
  ASSERT_TRUE(QMetaObject::invokeMethod(process, "selected", Q_ARG(int, 0),
                                        Q_ARG(QString, QStringLiteral("negative"))));
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  ASSERT_EQ(session.submitCount(), 2);
  drt = LastDrtWrite(session);
  ASSERT_NE(drt, nullptr);
  ASSERT_TRUE(drt->film_negative.has_value());
  EXPECT_EQ(*drt->film_negative, "kodak_portra_400");
  EXPECT_EQ(*drt->film_print_material, "fuji_ca_maxima");
  ASSERT_TRUE(WaitFor([&] { return print->isVisible(); }));

  // A select of the current process writes nothing.
  ASSERT_TRUE(QMetaObject::invokeMethod(process, "selected", Q_ARG(int, 0),
                                        Q_ARG(QString, QStringLiteral("negative"))));
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  EXPECT_EQ(session.submitCount(), 2);
}

TEST(EditorDisplayTransformSnapshotQmlTest, FilmBrowserShowsOnlyTheFilmsOfTheProcess) {
  DisplayTransformSession session(MakeFilmPrintSnapshot(), 0);
  AdjustmentStackHarness  harness(&session);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto* film  = harness.findModel<QQuickItem>(QStringLiteral("displayFilmNegativePicker"));
  auto* print = harness.findModel<QQuickItem>(QStringLiteral("displayFilmPrintMaterialPicker"));
  ASSERT_NE(film, nullptr);
  ASSERT_NE(print, nullptr);
  ASSERT_TRUE(WaitFor([&] {
    return film->property("currentValue").toString() == QStringLiteral("kodak_portra_400");
  }));

  auto values = PickerValues(film);
  ASSERT_EQ(values.size(), 30U);
  EXPECT_EQ(std::count(values.begin(), values.end(), "fuji_velvia_50"), 0);
  EXPECT_EQ(std::count(values.begin(), values.end(), "agfa_vista_100"), 1);
  EXPECT_TRUE(film->property("searchable").toBool());
  EXPECT_EQ(PickerShownValues(film).size(), 30U);
  EXPECT_EQ(PickerValues(print).size(), 19U);

  session.setAdjustmentSnapshot(MakeFilmPrintSnapshot(2.5, QStringLiteral("fuji_velvia_50")));
  ASSERT_TRUE(WaitFor([&] {
    return film->property("currentValue").toString() == QStringLiteral("fuji_velvia_50");
  }));
  values                                  = PickerValues(film);
  const std::vector<std::string> reversal = {"fuji_velvia_50", "fuji_velvia_100",
                                             "fuji_provia_100f", "fuji_astia_100f",
                                             "kodak_ektachrome_e100"};
  EXPECT_EQ(values, reversal);
  // Five films: no search field and no brand chips.
  EXPECT_FALSE(film->property("searchable").toBool());
  // The rows have a heading for each brand, Fujifilm first.
  const auto rows = film->property("rows").toList();
  ASSERT_FALSE(rows.isEmpty());
  EXPECT_EQ(rows.front().toMap().value(QStringLiteral("kind")).toString().toStdString(), "brand");
  EXPECT_EQ(rows.front().toMap().value(QStringLiteral("text")).toString().toStdString(),
            "FUJIFILM");
  EXPECT_EQ(PickerShownValues(film), reversal);
  EXPECT_FALSE(print->isVisible());
}

TEST(EditorDisplayTransformSnapshotQmlTest, FilmBrowserFiltersByBrandAndSearchAndPicksAFilm) {
  DisplayTransformSession session(MakeFilmPrintSnapshot(), 0);
  AdjustmentStackHarness  harness(&session);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto* film = harness.findModel<QQuickItem>(QStringLiteral("displayFilmNegativePicker"));
  ASSERT_NE(film, nullptr);
  ASSERT_TRUE(WaitFor([&] {
    return film->property("currentValue").toString() == QStringLiteral("kodak_portra_400");
  }));

  film->setProperty("expanded", true);
  ASSERT_TRUE(film->setProperty("brandFilter", QStringLiteral("Fujifilm")));
  auto shown = PickerShownValues(film);
  ASSERT_EQ(shown.size(), 9U);
  for (const auto& value : shown) {
    EXPECT_EQ(value.rfind("fuji_", 0), 0U) << value;
  }

  film->setProperty("brandFilter", QString());
  film->setProperty("query", QStringLiteral("portra"));
  shown                                 = PickerShownValues(film);
  const std::vector<std::string> portra = {"kodak_portra_400", "kodak_portra_160",
                                           "kodak_portra_800"};
  EXPECT_EQ(shown, portra);
  film->setProperty("query", QStringLiteral("no such film"));
  EXPECT_TRUE(PickerShownValues(film).empty());

  // A pick writes the film and closes the browser.
  film->setProperty("query", QString());
  ASSERT_TRUE(QMetaObject::invokeMethod(film, "pickValue",
                                        Q_ARG(QVariant, QStringLiteral("agfa_vista_100"))));
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  ASSERT_GE(session.submitCount(), 1);
  const auto* drt = LastDrtWrite(session);
  ASSERT_NE(drt, nullptr);
  ASSERT_TRUE(drt->film_negative.has_value());
  EXPECT_EQ(*drt->film_negative, "agfa_vista_100");
  EXPECT_FALSE(film->property("expanded").toBool());
  EXPECT_EQ(film->property("currentValue").toString().toStdString(), "agfa_vista_100");
}

TEST(EditorDisplayTransformSnapshotQmlTest,
     PostHalationSliderHidesForFilmPrintAndReturnsWithItsValue) {
  auto        open_drt = MakeOpenDrtSnapshot();
  QVariantMap halation_inner;
  halation_inner.insert(QStringLiteral("strength"), 0.25);
  QVariantMap halation_wrap;
  halation_wrap.insert(QStringLiteral("halation"), halation_inner);
  open_drt.insert(QStringLiteral("halation"), halation_wrap);

  DisplayTransformSession session(open_drt, 0);
  session.setActiveAdjustmentPanel(QStringLiteral("post"));
  AdjustmentStackHarness harness(&session);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto* slider = harness.findModel<QQuickItem>(QStringLiteral("postHalationSlider"));
  auto* model  = harness.findModel<QObject>(QStringLiteral("postHalationModel"));
  auto* grain  = harness.findModel<QQuickItem>(QStringLiteral("postFilmGrainSlider"));
  ASSERT_NE(slider, nullptr);
  ASSERT_NE(model, nullptr);
  ASSERT_NE(grain, nullptr);
  ASSERT_TRUE(WaitFor([&] { return slider->isVisible(); }));
  const double stored = model->property("value").toDouble();
  EXPECT_NE(stored, 0.0);

  auto film_print = MakeFilmPrintSnapshot();
  film_print.insert(QStringLiteral("halation"), halation_wrap);
  session.setAdjustmentSnapshot(film_print);
  ASSERT_TRUE(WaitFor([&] { return !slider->isVisible(); }));
  EXPECT_TRUE(grain->isVisible());
  EXPECT_DOUBLE_EQ(model->property("value").toDouble(), stored);
  EXPECT_EQ(session.submitCount(), 0);

  session.setAdjustmentSnapshot(open_drt);
  ASSERT_TRUE(WaitFor([&] { return slider->isVisible(); }));
  EXPECT_DOUBLE_EQ(model->property("value").toDouble(), stored);
  EXPECT_EQ(session.submitCount(), 0);
}

}  // namespace alcedo::ui::test

#include "editor_display_transform_snapshot_qml_test.moc"
