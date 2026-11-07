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
#include <QSettings>
#include <QTemporaryDir>
#include <QTest>
#include <QVariantMap>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <exception>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "app/editor_panel_projection.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/film_format.hpp"
#include "edit/graph/film_print_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/album_backend/editor_panel_presentation.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_submitter.hpp"
#include "ui/alcedo_main/album_backend/editor_behavior_preferences.hpp"
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
    field_keys_.push_back(std::move(fieldKey));
    settled_.push_back(settled);
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
  auto fieldKeys() const -> const std::vector<QString>& { return field_keys_; }
  auto settled() const -> const std::vector<bool>& { return settled_; }

 signals:
  void AdjustmentSnapshotChanged();
  void activeAdjustmentPanelChanged();

 private:
  QVariantMap                               snapshot_;
  quint64                                   revision_     = 0;
  int                                       submit_count_ = 0;
  QString                                   active_panel_ = QStringLiteral("display");
  std::vector<alcedo::EditorParameterWrite> writes_;
  std::vector<QString>                      field_keys_;
  std::vector<bool>                         settled_;
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
auto MakeFilmPrintSnapshot(double         halation       = 2.5,
                           const QString& negative       = QStringLiteral("kodak_portra_400"),
                           const QString& print_material = QStringLiteral("fuji_ca_maxima"))
    -> QVariantMap {
  QVariantMap film_print;
  film_print.insert(QStringLiteral("negative"), negative);
  film_print.insert(QStringLiteral("print_material"), print_material);
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

/// The snapshot entry of a DRT/Post scalar adjustment @p field with the Model value @p strength.
void InsertPostStrength(QVariantMap& snapshot, const QString& field, double strength) {
  QVariantMap inner;
  inner.insert(QStringLiteral("strength"), strength);
  QVariantMap wrap;
  wrap.insert(field, inner);
  snapshot.insert(field, wrap);
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

/// Index of the entry @p value in the entries of the enum model @p model, or -1.
auto EntryIndex(const QObject* model, const QString& value) -> int {
  const auto entries = model->property("entries").toList();
  for (int index = 0; index < entries.size(); ++index) {
    if (entries[index].toMap().value(QStringLiteral("value")).toString() == value) {
      return index;
    }
  }
  return -1;
}

TEST(EditorDisplayTransformSnapshotQmlTest,
     SelectingFilmPrintAppliesTheFilmPrintSettingsOfTheLastSettledFilmPrintWrite) {
  // The remembered settings live in QSettings: keep them in a temporary directory.
  QTemporaryDir settings_dir;
  ASSERT_TRUE(settings_dir.isValid());
  QSettings::setDefaultFormat(QSettings::IniFormat);
  QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir.path());
  QSettings{}.remove(QLatin1String(EditorBehaviorPreferences::kFilmPrintSettingsKey));
  EditorBehaviorPreferences preferences;

  DisplayTransformSession session(MakeFilmPrintSnapshot(), 0);
  AdjustmentStackHarness  harness(&session);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto* panel  = harness.findModel<QQuickItem>(QStringLiteral("editorAdjustmentPanel_display"));
  auto* method = harness.findModel<EditorAdjustmentEnumModel>(QStringLiteral("displayMethodModel"));
  auto* negative =
      harness.findModel<EditorAdjustmentEnumModel>(QStringLiteral("displayFilmNegativeModel"));
  ASSERT_NE(panel, nullptr);
  ASSERT_NE(method, nullptr);
  ASSERT_NE(negative, nullptr);
  panel->setProperty("editorBehavior", QVariant::fromValue(static_cast<QObject*>(&preferences)));
  ASSERT_TRUE(WaitFor([&] {
    return negative->property("currentValue").toString() == QStringLiteral("kodak_portra_400");
  }));

  // A settled film print write is remembered with every film print field of the panel.
  const int agfa = EntryIndex(negative, QStringLiteral("agfa_vista_100"));
  ASSERT_GE(agfa, 0);
  negative->selectIndex(agfa);
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  auto remembered = preferences.film_print_ui_value();
  EXPECT_EQ(remembered.value("negative").toString(), QStringLiteral("agfa_vista_100"));
  EXPECT_EQ(remembered.value("print_material").toString(), QStringLiteral("fuji_ca_maxima"));
  EXPECT_EQ(remembered.value("scene_light").toString(), QStringLiteral("tungsten"));
  EXPECT_DOUBLE_EQ(remembered.value("halation").toDouble(), 2.5);

  // A write of another method does not change them.
  method->selectIndex(EntryIndex(method, QStringLiteral("open_drt")));
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  EXPECT_EQ(preferences.film_print_ui_value(), remembered);

  // A document of another method shows the defaults of the film print. Its stored Film Grain
  // stays, so the selection of the film print writes only the method.
  auto open_drt = MakeOpenDrtSnapshot();
  InsertPostStrength(open_drt, QStringLiteral("film_grain"), 0.5);
  session.setAdjustmentSnapshot(open_drt);
  ASSERT_TRUE(WaitFor([&] {
    return method->property("currentValue").toString() == QStringLiteral("open_drt") &&
           negative->property("currentValue").toString() == QStringLiteral("kodak_5207");
  }));

  // Selecting Film Print writes the remembered settings in the same write.
  const int film_print = EntryIndex(method, QStringLiteral("film_print"));
  ASSERT_GE(film_print, 0);
  const int writes = session.submitCount();
  ASSERT_TRUE(QMetaObject::invokeMethod(panel, "selectMethod", Q_ARG(QVariant, film_print),
                                        Q_ARG(QVariant, QStringLiteral("film_print"))));
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  ASSERT_EQ(session.submitCount(), writes + 1);
  const auto* drt = std::get_if<alcedo::DrtParameterUpdate>(&session.writes().back());
  ASSERT_NE(drt, nullptr);
  ASSERT_TRUE(drt->method.has_value());
  EXPECT_EQ(*drt->method, alcedo::DrtMethod::FilmPrint);
  ASSERT_TRUE(drt->film_negative.has_value());
  EXPECT_EQ(*drt->film_negative, "agfa_vista_100");
  ASSERT_TRUE(drt->film_print_material.has_value());
  EXPECT_EQ(*drt->film_print_material, "fuji_ca_maxima");
  ASSERT_TRUE(drt->film_scene_light.has_value());
  EXPECT_EQ(*drt->film_scene_light, "tungsten");
  ASSERT_TRUE(drt->film_light_spread.has_value());
  EXPECT_FLOAT_EQ(*drt->film_light_spread, 0.5f);
  ASSERT_TRUE(drt->film_halation.has_value());
  EXPECT_FLOAT_EQ(*drt->film_halation, 2.5f);
  ASSERT_TRUE(drt->film_enlarger_flare.has_value());
  EXPECT_FLOAT_EQ(*drt->film_enlarger_flare, 0.05f);

  // Without remembered settings the selection writes the defaults: As Shot (issue #672).
  panel->setProperty("editorBehavior", QVariant::fromValue(static_cast<QObject*>(nullptr)));
  session.setAdjustmentSnapshot(open_drt);
  ASSERT_TRUE(WaitFor([&] {
    return method->property("currentValue").toString() == QStringLiteral("open_drt") &&
           negative->property("currentValue").toString() == QStringLiteral("kodak_5207");
  }));
  ASSERT_TRUE(QMetaObject::invokeMethod(panel, "selectMethod", Q_ARG(QVariant, film_print),
                                        Q_ARG(QVariant, QStringLiteral("film_print"))));
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  drt = std::get_if<alcedo::DrtParameterUpdate>(&session.writes().back());
  ASSERT_NE(drt, nullptr);
  ASSERT_TRUE(drt->film_negative.has_value());
  EXPECT_EQ(*drt->film_negative, "kodak_5207");
  ASSERT_TRUE(drt->film_scene_light.has_value());
  EXPECT_EQ(*drt->film_scene_light, "as_shot");
  QSettings::setDefaultFormat(QSettings::NativeFormat);
}

/// MakeFilmPrintSnapshot with scene light @p scene_light and the projected As Shot availability.
auto MakeFilmPrintSnapshotWithLight(const QString& scene_light, bool as_shot_available)
    -> QVariantMap {
  auto snapshot   = MakeFilmPrintSnapshot();
  auto odt_wrap   = snapshot.value(QStringLiteral("odt")).toMap();
  auto inner      = odt_wrap.value(QStringLiteral("odt")).toMap();
  auto film_print = inner.value(QStringLiteral("film_print")).toMap();
  film_print.insert(QStringLiteral("scene_light"), scene_light);
  inner.insert(QStringLiteral("film_print"), film_print);
  inner.insert(QStringLiteral("film_as_shot_available"), as_shot_available);
  odt_wrap.insert(QStringLiteral("odt"), inner);
  snapshot.insert(QStringLiteral("odt"), odt_wrap);
  return snapshot;
}

TEST(EditorDisplayTransformSnapshotQmlTest,
     AsShotIsHiddenWithNoRawWhiteBalanceAndAStoredAsShotStaysUntilTheUserPicksALight) {
  DisplayTransformSession session(MakeFilmPrintSnapshotWithLight(QStringLiteral("as_shot"), false),
                                  0);
  AdjustmentStackHarness  harness(&session);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto* light =
      harness.findModel<EditorAdjustmentEnumModel>(QStringLiteral("displayFilmSceneLightModel"));
  auto* negative =
      harness.findModel<EditorAdjustmentEnumModel>(QStringLiteral("displayFilmNegativeModel"));
  ASSERT_NE(light, nullptr);
  ASSERT_NE(negative, nullptr);
  ASSERT_TRUE(WaitFor([&] {
    return negative->property("currentValue").toString() == QStringLiteral("kodak_portra_400");
  }));

  // No RAW white balance: no As Shot entry, and the stored As Shot shows Reference, which it
  // renders.
  EXPECT_LT(EntryIndex(light, QStringLiteral("as_shot")), 0);
  EXPECT_EQ(light->property("currentValue").toString(), QStringLiteral("reference"));

  // Another film print write keeps the stored As Shot.
  negative->selectIndex(EntryIndex(negative, QStringLiteral("agfa_vista_100")));
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  const auto* drt = std::get_if<alcedo::DrtParameterUpdate>(&session.writes().back());
  ASSERT_NE(drt, nullptr);
  ASSERT_TRUE(drt->film_scene_light.has_value());
  EXPECT_EQ(*drt->film_scene_light, "as_shot");

  // A light that the user picks is written.
  light->selectIndex(EntryIndex(light, QStringLiteral("daylight")));
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  drt = std::get_if<alcedo::DrtParameterUpdate>(&session.writes().back());
  ASSERT_NE(drt, nullptr);
  ASSERT_TRUE(drt->film_scene_light.has_value());
  EXPECT_EQ(*drt->film_scene_light, "daylight");

  // A RAW whose as-shot temperature resolves shows As Shot.
  session.setAdjustmentSnapshot(MakeFilmPrintSnapshotWithLight(QStringLiteral("as_shot"), true));
  ASSERT_TRUE(WaitFor([&] {
    return light->property("currentValue").toString() == QStringLiteral("as_shot");
  }));
  EXPECT_GE(EntryIndex(light, QStringLiteral("as_shot")), 0);
}

TEST(EditorDisplayTransformSnapshotQmlTest, NegativeScanHidesTheEnlargerFlareAndKeepsItsValue) {
  DisplayTransformSession session(MakeFilmPrintSnapshot(), 0);
  AdjustmentStackHarness  harness(&session);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto* flare_slider =
      harness.findModel<QQuickItem>(QStringLiteral("displayFilmEnlargerFlareSlider"));
  auto* halation_slider =
      harness.findModel<QQuickItem>(QStringLiteral("displayFilmHalationSlider"));
  auto* material_picker =
      harness.findModel<QQuickItem>(QStringLiteral("displayFilmPrintMaterialPicker"));
  auto* material = harness.findModel<EditorAdjustmentEnumModel>(
      QStringLiteral("displayFilmPrintMaterialModel"));
  auto* halation =
      harness.findModel<EditorAdjustmentValueModel>(QStringLiteral("displayFilmHalationModel"));
  ASSERT_NE(flare_slider, nullptr);
  ASSERT_NE(halation_slider, nullptr);
  ASSERT_NE(material_picker, nullptr);
  ASSERT_NE(material, nullptr);
  ASSERT_NE(halation, nullptr);
  ASSERT_TRUE(WaitFor([&] { return flare_slider->isVisible(); }));

  // The Print material list has Negative Scan first, before Cine Look.
  const auto entries = material->property("entries").toList();
  ASSERT_GE(entries.size(), 2);
  EXPECT_EQ(entries[0].toMap().value("value").toString().toStdString(), "negative_scan");
  EXPECT_EQ(entries[0].toMap().value("label").toString().toStdString(), "Negative Scan");
  EXPECT_EQ(entries[1].toMap().value("value").toString().toStdString(), "cine_look");
  const auto browser = PickerValues(material_picker);
  ASSERT_FALSE(browser.empty());
  EXPECT_EQ(browser.front(), "negative_scan");

  session.setAdjustmentSnapshot(MakeFilmPrintSnapshot(2.5, QStringLiteral("kodak_portra_400"),
                                                      QStringLiteral("negative_scan")));
  ASSERT_TRUE(WaitFor([&] { return !flare_slider->isVisible(); }));
  EXPECT_TRUE(halation_slider->isVisible());
  EXPECT_TRUE(material_picker->isVisible());

  // A write of another slider keeps the stored Enlarger flare.
  halation->editValue(3.0);
  halation->commitImmediately();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  ASSERT_GE(session.submitCount(), 1);
  const auto* drt = std::get_if<alcedo::DrtParameterUpdate>(&session.writes().back());
  ASSERT_NE(drt, nullptr);
  ASSERT_TRUE(drt->film_print_material.has_value());
  EXPECT_EQ(*drt->film_print_material, "negative_scan");
  ASSERT_TRUE(drt->film_enlarger_flare.has_value());
  EXPECT_FLOAT_EQ(*drt->film_enlarger_flare, 0.05f);

  // A reversal film hides the print material, and its loupe flare shows with a stored Negative
  // Scan.
  session.setAdjustmentSnapshot(MakeFilmPrintSnapshot(2.5, QStringLiteral("fuji_velvia_50"),
                                                      QStringLiteral("negative_scan")));
  ASSERT_TRUE(WaitFor([&] { return !material_picker->isVisible(); }));
  EXPECT_TRUE(flare_slider->isVisible());
  EXPECT_EQ(material->property("currentValue").toString().toStdString(), "negative_scan");
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
  // Negative Scan and the 19 print materials of the bundle.
  EXPECT_EQ(PickerValues(print).size(), 20U);

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

  // A pick writes the film and keeps the browser open, with the film highlighted, so the next pick
  // compares the films (issue 661).
  film->setProperty("query", QString());
  ASSERT_TRUE(QMetaObject::invokeMethod(film, "pickValue",
                                        Q_ARG(QVariant, QStringLiteral("agfa_vista_100"))));
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  ASSERT_GE(session.submitCount(), 1);
  const auto* drt = LastDrtWrite(session);
  ASSERT_NE(drt, nullptr);
  ASSERT_TRUE(drt->film_negative.has_value());
  EXPECT_EQ(*drt->film_negative, "agfa_vista_100");
  ASSERT_TRUE(WaitFor([&] {
    return film->property("currentValue").toString() == QStringLiteral("agfa_vista_100");
  }));
  EXPECT_TRUE(film->property("expanded").toBool());
  QVariant highlighted_row;
  ASSERT_TRUE(QMetaObject::invokeMethod(film, "rowOfValue", Q_RETURN_ARG(QVariant, highlighted_row),
                                        Q_ARG(QVariant, QStringLiteral("agfa_vista_100"))));
  EXPECT_EQ(film->property("highlightedRow").toInt(), highlighted_row.toInt());

  // A second pick also keeps it open. Escape closes it, and a click on the card opens and closes
  // it.
  ASSERT_TRUE(QMetaObject::invokeMethod(film, "pickValue",
                                        Q_ARG(QVariant, QStringLiteral("kodak_gold_200"))));
  ASSERT_TRUE(WaitFor([&] {
    return film->property("currentValue").toString() == QStringLiteral("kodak_gold_200");
  }));
  EXPECT_TRUE(film->property("expanded").toBool());
  QVariant handled;
  ASSERT_TRUE(QMetaObject::invokeMethod(film, "handleBrowserKey", Q_RETURN_ARG(QVariant, handled),
                                        Q_ARG(QVariant, static_cast<int>(Qt::Key_Escape))));
  EXPECT_TRUE(handled.toBool());
  EXPECT_FALSE(film->property("expanded").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(film, "toggleExpanded"));
  EXPECT_TRUE(film->property("expanded").toBool());
  ASSERT_TRUE(QMetaObject::invokeMethod(film, "toggleExpanded"));
  EXPECT_FALSE(film->property("expanded").toBool());
}

/// One setting of Negative Scan in the panel: its model, a value other than the default, and the
/// field of the write.
struct ScanControl {
  const char* name;  ///< The part of `displayFilm<name>Model`
  bool        option = false;
  QVariant    value;
  std::function<std::optional<QVariant>(const alcedo::DrtParameterUpdate&)> field;
};

auto ScanControls() -> std::vector<ScanControl> {
  const auto number = [](auto member) {
    return [member](const alcedo::DrtParameterUpdate& update) -> std::optional<QVariant> {
      const auto& value = update.*member;
      if (!value.has_value()) return std::nullopt;
      return QVariant(static_cast<double>(*value));
    };
  };
  const auto text = [](auto member) {
    return [member](const alcedo::DrtParameterUpdate& update) -> std::optional<QVariant> {
      const auto& value = update.*member;
      if (!value.has_value()) return std::nullopt;
      return QVariant(QString::fromStdString(*value));
    };
  };
  using U = alcedo::DrtParameterUpdate;
  return {
      {"ScanAuto", true, QStringLiteral("warm"), text(&U::film_scan_auto)},
      {"ScanAutoStrength", false, 0.75, number(&U::film_scan_auto_strength)},
      {"ScanAutoDensity", false, 0.25, number(&U::film_scan_auto_density)},
      {"ScanContrast", false, 1.25, number(&U::film_scan_contrast)},
      {"ScanExposure", false, -0.75, number(&U::film_scan_exposure_stops)},
      {"ScanCyan", false, 0.05, number(&U::film_scan_cyan)},
      {"ScanMagenta", false, -0.03, number(&U::film_scan_magenta)},
      {"ScanYellow", false, 0.12, number(&U::film_scan_yellow)},
      {"ScanBlack", false, 0.5, number(&U::film_scan_black)},
      {"ScannerRed", false, 630.0, number(&U::film_scanner_red_nm)},
      {"ScannerGreen", false, 530.0, number(&U::film_scanner_green_nm)},
      {"ScannerBlue", false, 460.0, number(&U::film_scanner_blue_nm)},
      {"ScannerWidth", false, 60.0, number(&U::film_scanner_width_nm)},
      {"ScannerLight", true, QStringLiteral("equal_energy"), text(&U::film_scanner_light)},
  };
}

/// A Negative Scan document of kodak_5207 under As Shot, with @p scan.
auto NegativeScanDocument(const alcedo::DrtParameterUpdate& scan) -> alcedo::PipelineDocument {
  auto                       document = alcedo::CreateDefaultPipelineDocument();
  alcedo::DrtParameterUpdate update;
  update.method              = alcedo::DrtMethod::FilmPrint;
  update.film_negative       = std::string{"kodak_5207"};
  update.film_print_material = std::string{alcedo::kFilmNegativeScanId};
  update.film_scene_light    = std::string{alcedo::kFilmAsShotLightId};
  document.Drt()->Params().ApplyUpdate(update);
  document.Drt()->Params().ApplyUpdate(scan);
  return document;
}

/// The panel snapshot of @p document, from the projection of the editor (issue 661: the
/// projection has every Negative Scan key, also at its default).
auto ProjectedSnapshot(const alcedo::PipelineDocument& document) -> QVariantMap {
  alcedo::EditorPanelProjection projection;
  std::string                   error;
  EXPECT_TRUE(alcedo::ProjectCurrentPanelFields(document, 0, &projection, &error)) << error;
  return alcedo::ui::PanelProjectionToVariantMap(projection);
}

/// The value of a scan model of the panel: the value of an option, or the number.
auto ScanModelValue(QObject* model, bool option) -> QVariant {
  return option ? QVariant(model->property("currentValue").toString())
                : QVariant(model->property("value").toDouble());
}

auto SameValue(const QVariant& a, const QVariant& b) -> bool {
  if (a.typeId() == QMetaType::QString || b.typeId() == QMetaType::QString) {
    return a.toString() == b.toString();
  }
  return std::abs(a.toDouble() - b.toDouble()) < 1e-4;
}

TEST(EditorDisplayTransformSnapshotQmlTest, NegativeScanShowsItsSettingsAndTheOpenDrtControls) {
  DisplayTransformSession session(MakeFilmPrintSnapshot(), 0);
  AdjustmentStackHarness  harness(&session);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto* scan_controls =
      harness.findModel<QQuickItem>(QStringLiteral("displayNegativeScanControls"));
  auto* open_drt =
      harness.findModel<QQuickItem>(QStringLiteral("displayNegativeScanOpenDrtControls"));
  auto* look_combo =
      harness.findModel<QQuickItem>(QStringLiteral("displayNegativeScanOpenDrtLookCombo"));
  auto* encoding = harness.findModel<QQuickItem>(QStringLiteral("displayEncodingSpaceCombo"));
  auto* halation_slider =
      harness.findModel<QQuickItem>(QStringLiteral("displayFilmHalationSlider"));
  ASSERT_NE(scan_controls, nullptr);
  ASSERT_NE(open_drt, nullptr);
  ASSERT_NE(look_combo, nullptr);
  ASSERT_NE(encoding, nullptr);
  ASSERT_NE(halation_slider, nullptr);
  // A print has no scan settings, no OpenDRT settings, and no encodings.
  ASSERT_TRUE(WaitFor([&] { return halation_slider->isVisible(); }));
  EXPECT_FALSE(scan_controls->isVisible());
  EXPECT_FALSE(open_drt->isVisible());
  EXPECT_FALSE(encoding->isVisible());

  session.setAdjustmentSnapshot(ProjectedSnapshot(NegativeScanDocument({})));
  ASSERT_TRUE(WaitFor([&] { return scan_controls->isVisible(); }));
  ASSERT_TRUE(WaitFor([&] { return open_drt->isVisible() && look_combo->isVisible(); }));
  EXPECT_TRUE(encoding->isVisible());
  // Each setting has its control in the group.
  for (const auto& control : ScanControls()) {
    const auto item = QStringLiteral("displayFilm%1%2")
                          .arg(QString::fromLatin1(control.name),
                               control.option ? QStringLiteral("Combo") : QStringLiteral("Slider"));
    auto* shown = harness.findModel<QQuickItem>(item);
    ASSERT_NE(shown, nullptr) << item.toStdString();
    EXPECT_TRUE(shown->isVisible()) << item.toStdString();
  }

  // A print hides the controls again.
  session.setAdjustmentSnapshot(MakeFilmPrintSnapshot());
  ASSERT_TRUE(WaitFor([&] { return !scan_controls->isVisible() && !open_drt->isVisible(); }));
}

TEST(EditorDisplayTransformSnapshotQmlTest, EachNegativeScanControlWritesItsKeyWithTheSelection) {
  DisplayTransformSession session(ProjectedSnapshot(NegativeScanDocument({})), 0);
  AdjustmentStackHarness  harness(&session);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto* scan_controls =
      harness.findModel<QQuickItem>(QStringLiteral("displayNegativeScanControls"));
  ASSERT_NE(scan_controls, nullptr);
  ASSERT_TRUE(WaitFor([&] { return scan_controls->isVisible(); }));
  for (const auto& control : ScanControls()) {
    SCOPED_TRACE(control.name);
    const auto name  = QStringLiteral("displayFilm%1Model").arg(QString::fromLatin1(control.name));
    const auto count = session.submitCount();
    if (control.option) {
      auto* model = harness.findModel<EditorAdjustmentEnumModel>(name);
      ASSERT_NE(model, nullptr);
      const auto entries = model->property("entries").toList();
      int        index   = -1;
      for (int i = 0; i < entries.size(); ++i) {
        if (entries[i].toMap().value("value") == control.value) index = i;
      }
      ASSERT_GE(index, 0);
      model->selectIndex(index);
    } else {
      auto* model = harness.findModel<EditorAdjustmentValueModel>(name);
      ASSERT_NE(model, nullptr);
      model->editValue(control.value.toDouble());
      model->commitImmediately();
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    ASSERT_GT(session.submitCount(), count);
    const auto* drt = LastDrtWrite(session);
    ASSERT_NE(drt, nullptr);
    const auto written = control.field(*drt);
    ASSERT_TRUE(written.has_value());
    EXPECT_TRUE(SameValue(*written, control.value)) << written->toString().toStdString();
    ASSERT_TRUE(drt->film_print_material.has_value());
    EXPECT_EQ(*drt->film_print_material, "negative_scan");
    ASSERT_TRUE(drt->film_negative.has_value());
    EXPECT_EQ(*drt->film_negative, "kodak_5207");
  }
}

TEST(EditorDisplayTransformSnapshotQmlTest, NegativeScanSettingsFollowTheProjectionOfEachDocument) {
  // A document with every setting away from its default, then a document with the defaults (the
  // undo of those settings): each control shows the value of its document, so a later Halation
  // write carries the defaults. The projection omitted the keys before issue 661, and the panel
  // then kept the values of the first document.
  alcedo::DrtParameterUpdate scan;
  scan.film_scan_auto           = std::string{"warm"};
  scan.film_scan_auto_strength  = 0.75f;
  scan.film_scan_auto_density   = 0.25f;
  scan.film_scan_contrast       = 1.25f;
  scan.film_scan_exposure_stops = -0.75f;
  scan.film_scan_cyan           = 0.05f;
  scan.film_scan_magenta        = -0.03f;
  scan.film_scan_yellow         = 0.12f;
  scan.film_scan_black          = 0.5f;
  scan.film_scanner_red_nm      = 630.0f;
  scan.film_scanner_green_nm    = 530.0f;
  scan.film_scanner_blue_nm     = 460.0f;
  scan.film_scanner_width_nm    = 60.0f;
  scan.film_scanner_light       = std::string{"equal_energy"};
  DisplayTransformSession session(ProjectedSnapshot(NegativeScanDocument(scan)), 0);
  AdjustmentStackHarness  harness(&session);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  const auto controls = ScanControls();
  const auto model_of = [&](const ScanControl& control) {
    return harness.findModel<QObject>(
        QStringLiteral("displayFilm%1Model").arg(QString::fromLatin1(control.name)));
  };
  for (const auto& control : controls) {
    SCOPED_TRACE(control.name);
    auto* model = model_of(control);
    ASSERT_NE(model, nullptr);
    ASSERT_TRUE(
        WaitFor([&] { return SameValue(ScanModelValue(model, control.option), control.value); }))
        << ScanModelValue(model, control.option).toString().toStdString();
  }

  session.setAdjustmentSnapshot(ProjectedSnapshot(NegativeScanDocument({})));
  const alcedo::FilmNegativeScanParams defaults;
  alcedo::DrtParameterUpdate           at_default;
  at_default.film_scan_auto           = defaults.auto_colour;
  at_default.film_scan_auto_strength  = defaults.auto_strength;
  at_default.film_scan_auto_density   = defaults.auto_density;
  at_default.film_scan_contrast       = defaults.contrast;
  at_default.film_scan_exposure_stops = defaults.exposure_stops;
  at_default.film_scan_cyan           = defaults.cyan;
  at_default.film_scan_magenta        = defaults.magenta;
  at_default.film_scan_yellow         = defaults.yellow;
  at_default.film_scan_black          = defaults.black;
  at_default.film_scanner_red_nm      = defaults.scanner_red_nm;
  at_default.film_scanner_green_nm    = defaults.scanner_green_nm;
  at_default.film_scanner_blue_nm     = defaults.scanner_blue_nm;
  at_default.film_scanner_width_nm    = defaults.scanner_width_nm;
  at_default.film_scanner_light       = defaults.scanner_light;
  for (const auto& control : controls) {
    SCOPED_TRACE(control.name);
    const auto expected = *control.field(at_default);
    auto*      model    = model_of(control);
    ASSERT_TRUE(WaitFor([&] { return SameValue(ScanModelValue(model, control.option), expected); }))
        << ScanModelValue(model, control.option).toString().toStdString();
  }

  auto* halation =
      harness.findModel<EditorAdjustmentValueModel>(QStringLiteral("displayFilmHalationModel"));
  ASSERT_NE(halation, nullptr);
  halation->editValue(3.0);
  halation->commitImmediately();
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
  const auto* drt = LastDrtWrite(session);
  ASSERT_NE(drt, nullptr);
  for (const auto& control : controls) {
    SCOPED_TRACE(control.name);
    const auto written = control.field(*drt);
    ASSERT_TRUE(written.has_value());
    EXPECT_TRUE(SameValue(*written, *control.field(at_default)))
        << written->toString().toStdString();
  }
}

// The film print takes its halation from the film layers and its grain in the negative, with the
// Film Grain slider and the Format selector of the Display Transform page. So the Post Processing
// page has no Texture section with the film print, and it returns with its values with another
// method.
TEST(EditorDisplayTransformSnapshotQmlTest,
     PostTextureSectionHidesForFilmPrintAndReturnsWithItsValues) {
  auto open_drt = MakeOpenDrtSnapshot();
  InsertPostStrength(open_drt, QStringLiteral("halation"), 0.25);
  InsertPostStrength(open_drt, QStringLiteral("film_grain"), 0.4);

  DisplayTransformSession session(open_drt, 0);
  session.setActiveAdjustmentPanel(QStringLiteral("post"));
  AdjustmentStackHarness harness(&session);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto* section =
      harness.findModel<QQuickItem>(QStringLiteral("editorAdjustmentGroupShell_post_texture"));
  auto* slider = harness.findModel<QQuickItem>(QStringLiteral("postHalationSlider"));
  auto* model  = harness.findModel<QObject>(QStringLiteral("postHalationModel"));
  auto* grain  = harness.findModel<QQuickItem>(QStringLiteral("postFilmGrainSlider"));
  auto* grain_model = harness.findModel<QObject>(QStringLiteral("postFilmGrainModel"));
  auto* format = harness.findModel<QQuickItem>(QStringLiteral("postFilmFormatControl"));
  ASSERT_NE(section, nullptr);
  ASSERT_NE(slider, nullptr);
  ASSERT_NE(model, nullptr);
  ASSERT_NE(grain, nullptr);
  ASSERT_NE(grain_model, nullptr);
  ASSERT_NE(format, nullptr);
  ASSERT_TRUE(WaitFor([&] { return slider->isVisible(); }));
  EXPECT_TRUE(grain->isVisible());
  EXPECT_TRUE(format->isVisible());
  const double stored       = model->property("value").toDouble();
  const double stored_grain = grain_model->property("value").toDouble();
  EXPECT_NE(stored, 0.0);
  EXPECT_DOUBLE_EQ(stored_grain, 40.0);

  auto film_print = MakeFilmPrintSnapshot();
  InsertPostStrength(film_print, QStringLiteral("halation"), 0.25);
  InsertPostStrength(film_print, QStringLiteral("film_grain"), 0.4);
  session.setAdjustmentSnapshot(film_print);
  ASSERT_TRUE(WaitFor([&] { return !section->isVisible(); }));
  EXPECT_FALSE(slider->isVisible());
  EXPECT_FALSE(grain->isVisible());
  EXPECT_FALSE(format->isVisible());
  EXPECT_DOUBLE_EQ(model->property("value").toDouble(), stored);
  EXPECT_DOUBLE_EQ(grain_model->property("value").toDouble(), stored_grain);
  EXPECT_EQ(session.submitCount(), 0);

  session.setAdjustmentSnapshot(open_drt);
  ASSERT_TRUE(WaitFor([&] { return section->isVisible(); }));
  EXPECT_TRUE(slider->isVisible());
  EXPECT_TRUE(grain->isVisible());
  EXPECT_DOUBLE_EQ(model->property("value").toDouble(), stored);
  EXPECT_DOUBLE_EQ(grain_model->property("value").toDouble(), stored_grain);
  EXPECT_EQ(session.submitCount(), 0);
}

/// The panel snapshot of a document with the format @p format: with @p film_print, a film print of
/// Kodak 5207 on 2383, else the default OpenDRT.
auto FilmFormatSnapshot(alcedo::FilmFormat format, bool film_print = true) -> QVariantMap {
  auto document = alcedo::CreateDefaultPipelineDocument();
  if (film_print) {
    alcedo::DrtParameterUpdate update;
    update.method              = alcedo::DrtMethod::FilmPrint;
    update.film_negative       = std::string{"kodak_5207"};
    update.film_print_material = std::string{"kodak_2383"};
    document.Drt()->Params().ApplyUpdate(update);
  }
  document.Drt()->Params().ApplyFilmFormat(format);
  return ProjectedSnapshot(document);
}

// Plan 'film_grain_format_and_stock_plan.md', sections 7.1 and 7.2: the Post Processing page and
// the Display Transform page show the same stored film format. The Post Processing page shows it
// with a method other than the film print.
TEST(EditorDisplayTransformSnapshotQmlTest, FilmFormatSelectorsOfBothPagesShowTheStoredFormat) {
  DisplayTransformSession session(FilmFormatSnapshot(alcedo::FilmFormat::Film16mm, false), 0);
  session.setActiveAdjustmentPanel(QStringLiteral("post"));
  AdjustmentStackHarness harness(&session);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto* post_model =
      harness.findModel<EditorAdjustmentEnumModel>(QStringLiteral("postFilmFormatModel"));
  auto* post_control = harness.findModel<QQuickItem>(QStringLiteral("postFilmFormatControl"));
  ASSERT_NE(post_model, nullptr);
  ASSERT_NE(post_control, nullptr);
  ASSERT_TRUE(WaitFor([&] { return post_model->currentValue() == QStringLiteral("16mm"); }));
  EXPECT_EQ(post_control->property("currentValue").toString(), QStringLiteral("16mm"));
  EXPECT_TRUE(post_control->isVisible());
  const auto entries = post_model->entries();
  ASSERT_EQ(entries.size(), 3);
  EXPECT_EQ(entries[0].toMap().value("value").toString(), QStringLiteral("16mm"));
  EXPECT_EQ(entries[1].toMap().value("value").toString(), QStringLiteral("35mm"));
  EXPECT_EQ(entries[2].toMap().value("value").toString(), QStringLiteral("70mm"));
  EXPECT_EQ(post_model->defaultIndex(), 1);

  session.setActiveAdjustmentPanel(QStringLiteral("display"));
  auto* display_model = static_cast<EditorAdjustmentEnumModel*>(nullptr);
  ASSERT_TRUE(WaitFor([&] {
    display_model =
        harness.findModel<EditorAdjustmentEnumModel>(QStringLiteral("displayFilmFormatModel"));
    return display_model != nullptr && display_model->currentValue() == QStringLiteral("16mm");
  }));
  auto* display_control = harness.findModel<QQuickItem>(QStringLiteral("displayFilmFormatControl"));
  ASSERT_NE(display_control, nullptr);
  EXPECT_EQ(display_control->property("currentValue").toString(), QStringLiteral("16mm"));

  // A new projection moves both selectors and writes nothing.
  session.setAdjustmentSnapshot(FilmFormatSnapshot(alcedo::FilmFormat::Film70mm));
  ASSERT_TRUE(WaitFor([&] { return display_model->currentValue() == QStringLiteral("70mm"); }));
  ASSERT_TRUE(WaitFor([&] { return display_control->isVisible(); }));
  session.setActiveAdjustmentPanel(QStringLiteral("post"));
  ASSERT_TRUE(WaitFor([&] {
    post_model =
        harness.findModel<EditorAdjustmentEnumModel>(QStringLiteral("postFilmFormatModel"));
    return post_model != nullptr && post_model->currentValue() == QStringLiteral("70mm");
  }));
  EXPECT_EQ(session.submitCount(), 0);
}

TEST(EditorDisplayTransformSnapshotQmlTest, FilmFormatSelectionWritesOneSettledFilmFormatWrite) {
  for (const auto* panel : {"post", "display"}) {
    SCOPED_TRACE(panel);
    // The Post Processing page shows the Format with a method other than the film print, and
    // the Display Transform page with the film print.
    const bool              film_print = std::string_view{panel} == "display";
    DisplayTransformSession session(FilmFormatSnapshot(alcedo::FilmFormat::Film35mm, film_print),
                                    0);
    session.setActiveAdjustmentPanel(QString::fromLatin1(panel));
    AdjustmentStackHarness harness(&session);
    ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
    const auto prefix  = QString::fromLatin1(panel);
    auto*      model   = harness.findModel<EditorAdjustmentEnumModel>(prefix + "FilmFormatModel");
    auto*      control = harness.findModel<QQuickItem>(prefix + "FilmFormatControl");
    ASSERT_NE(model, nullptr);
    ASSERT_NE(control, nullptr);
    ASSERT_TRUE(WaitFor([&] { return model->currentValue() == QStringLiteral("35mm"); }));

    // The selection of a segment, as a click gives it.
    ASSERT_TRUE(QMetaObject::invokeMethod(control, "selected", Q_ARG(int, 0),
                                          Q_ARG(QString, QStringLiteral("16mm"))));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    ASSERT_EQ(session.submitCount(), 1);
    EXPECT_EQ(session.fieldKeys().back(), QStringLiteral("film_format"));
    EXPECT_TRUE(session.settled().back());
    const auto* write = std::get_if<alcedo::DrtFilmFormatUpdate>(&session.writes().back());
    ASSERT_NE(write, nullptr);
    EXPECT_EQ(write->format, alcedo::FilmFormat::Film16mm);
    EXPECT_EQ(control->property("currentValue").toString(), QStringLiteral("16mm"));

    // The selected segment again writes nothing.
    ASSERT_TRUE(QMetaObject::invokeMethod(control, "selected", Q_ARG(int, 0),
                                          Q_ARG(QString, QStringLiteral("16mm"))));
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    EXPECT_EQ(session.submitCount(), 1);
  }
}

// With the film print, the Film Grain slider is on the Display Transform page directly under the
// Format selector, with the stored value. No page shows a caption of the granularity.
TEST(EditorDisplayTransformSnapshotQmlTest, FilmPrintShowsFilmGrainUnderTheFormat) {
  auto snapshot = MakeFilmPrintSnapshot();
  InsertPostStrength(snapshot, QStringLiteral("film_grain"), 0.6);
  DisplayTransformSession session(snapshot, 0);
  AdjustmentStackHarness  harness(&session);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto* grain  = harness.findModel<QQuickItem>(QStringLiteral("displayFilmGrainSlider"));
  auto* format = harness.findModel<QQuickItem>(QStringLiteral("displayFilmFormatControl"));
  auto* print  = harness.findModel<QQuickItem>(QStringLiteral("displayFilmPrintMaterialPicker"));
  auto* model =
      harness.findModel<EditorAdjustmentValueModel>(QStringLiteral("displayFilmGrainModel"));
  ASSERT_NE(grain, nullptr);
  ASSERT_NE(format, nullptr);
  ASSERT_NE(print, nullptr);
  ASSERT_NE(model, nullptr);
  ASSERT_TRUE(WaitFor([&] { return grain->isVisible() && model->value() == 60.0; }))
      << model->value();
  // 100 % is the datasheet granularity; the slider goes to 200 % (decision U3).
  EXPECT_DOUBLE_EQ(model->maximum(), 200.0);
  const auto top = [&](QQuickItem* item) { return item->mapToScene(QPointF()).y(); };
  EXPECT_GT(top(grain), top(format) + format->height() - 1.0);
  EXPECT_LT(top(grain), top(print));
  EXPECT_EQ(harness.findModel<QQuickItem>(QStringLiteral("postFilmGrainStockCaption")), nullptr);

  // Another method has no film print controls, so the slider is on the Post Processing page.
  session.setAdjustmentSnapshot(MakeOpenDrtSnapshot());
  ASSERT_TRUE(WaitFor([&] { return !grain->isVisible(); }));
  session.setActiveAdjustmentPanel(QStringLiteral("post"));
  ASSERT_TRUE(WaitFor([&] {
    auto* post = harness.findModel<QQuickItem>(QStringLiteral("postFilmGrainSlider"));
    return post != nullptr && post->isVisible();
  }));
  EXPECT_EQ(harness.findModel<QQuickItem>(QStringLiteral("postFilmGrainStockCaption")), nullptr);
  EXPECT_EQ(session.submitCount(), 0);
}

/// Select method Film Print with the method track, as a click does.
void ClickFilmPrintMethod(const AdjustmentStackHarness& harness) {
  auto* track = harness.findModel<QQuickItem>(QStringLiteral("displayMethodTrack"));
  ASSERT_NE(track, nullptr);
  ASSERT_TRUE(QMetaObject::invokeMethod(track, "selected", Q_ARG(int, 2),
                                        Q_ARG(QString, QStringLiteral("film_print"))));
  QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
}

// A selection of the film print turns on Film Grain at the datasheet granularity (100 %) when its
// stored strength is 0: one settled write of the method, then one settled write of Film Grain.
TEST(EditorDisplayTransformSnapshotQmlTest, SelectingFilmPrintTurnsOnFilmGrainAtTheDatasheet) {
  DisplayTransformSession session(MakeOpenDrtSnapshot(), 0);
  AdjustmentStackHarness  harness(&session);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto* model =
      harness.findModel<EditorAdjustmentValueModel>(QStringLiteral("displayFilmGrainModel"));
  ASSERT_NE(model, nullptr);
  ASSERT_TRUE(WaitFor([&] {
    return harness.findModel<QObject>(QStringLiteral("displayMethodModel"))
               ->property("currentValue")
               .toString() == QStringLiteral("open_drt");
  }));
  EXPECT_EQ(model->value(), 0.0);

  ClickFilmPrintMethod(harness);
  ASSERT_GE(session.submitCount(), 2);
  EXPECT_EQ(session.fieldKeys().front(), QStringLiteral("odt"));
  EXPECT_TRUE(session.settled().front());
  const auto* method = std::get_if<alcedo::DrtParameterUpdate>(&session.writes().front());
  ASSERT_NE(method, nullptr);
  ASSERT_TRUE(method->method.has_value());
  EXPECT_EQ(*method->method, alcedo::DrtMethod::FilmPrint);
  // After the method, Film Grain only, and the last write is settled at 1.0 (100 %).
  int settled_grain = 0;
  for (std::size_t index = 1; index < session.writes().size(); ++index) {
    EXPECT_EQ(session.fieldKeys()[index], QStringLiteral("film_grain")) << index;
    settled_grain += session.settled()[index] ? 1 : 0;
  }
  EXPECT_EQ(settled_grain, 1);
  EXPECT_TRUE(session.settled().back());
  const auto* grain = std::get_if<alcedo::EditorScalarWrite>(&session.writes().back());
  ASSERT_NE(grain, nullptr);
  EXPECT_FLOAT_EQ(grain->value, 1.0f);
  EXPECT_DOUBLE_EQ(model->value(), 100.0);
}

// A stored Film Grain strength stays: the selection of the film print writes only the method.
// An old document with a strength and no film_format reads the format 35mm.
TEST(EditorDisplayTransformSnapshotQmlTest, SelectingFilmPrintKeepsAStoredFilmGrain) {
  auto snapshot = MakeOpenDrtSnapshot();
  InsertPostStrength(snapshot, QStringLiteral("film_grain"), 0.3);
  DisplayTransformSession session(snapshot, 0);
  AdjustmentStackHarness  harness(&session);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto* model =
      harness.findModel<EditorAdjustmentValueModel>(QStringLiteral("displayFilmGrainModel"));
  auto* format =
      harness.findModel<EditorAdjustmentEnumModel>(QStringLiteral("displayFilmFormatModel"));
  ASSERT_NE(model, nullptr);
  ASSERT_NE(format, nullptr);
  ASSERT_TRUE(WaitFor([&] { return model->value() == 30.0; })) << model->value();
  EXPECT_EQ(format->currentValue(), QStringLiteral("35mm"));

  ClickFilmPrintMethod(harness);
  ASSERT_EQ(session.submitCount(), 1);
  EXPECT_EQ(session.fieldKeys().back(), QStringLiteral("odt"));
  EXPECT_DOUBLE_EQ(model->value(), 30.0);
}

}  // namespace alcedo::ui::test

#include "editor_display_transform_snapshot_qml_test.moc"
