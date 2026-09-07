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
#include <QQuickStyle>
#include <QQuickItem>
#include <QQuickWindow>
#include <QVariantMap>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <variant>
#include <vector>

#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_submitter.hpp"
#include "ui/alcedo_main/app_theme.hpp"

namespace alcedo::ui::test {
namespace {

class DisplayTransformSession final : public QObject, public IEditorAdjustmentSubmitter {
  Q_OBJECT
  Q_PROPERTY(QVariantMap adjustmentSnapshot READ adjustmentSnapshot NOTIFY AdjustmentSnapshotChanged)
  Q_PROPERTY(quint64 snapshotRevision READ snapshotRevision NOTIFY AdjustmentSnapshotChanged)
  Q_PROPERTY(QString activeAdjustmentPanel READ activeAdjustmentPanel
             WRITE setActiveAdjustmentPanel NOTIFY activeAdjustmentPanelChanged)

 public:
  explicit DisplayTransformSession(QVariantMap snapshot, quint64 revision)
      : snapshot_(std::move(snapshot)), revision_(revision) {}

  auto adjustmentSnapshot() const -> QVariantMap { return snapshot_; }
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
  QVariantMap snapshot_;
  quint64     revision_     = 0;
  int         submit_count_ = 0;
  QString     active_panel_ = QStringLiteral("display");
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
    root_.reset(qobject_cast<QQuickItem*>(component.createWithInitialProperties(initial_properties)));
    if (!root_) {
      errors_ = component.errors();
      return;
    }

    root_->setParentItem(window_.contentItem());
    window_.resize(400, 700);
    window_.show();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 200);
  }

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

auto MakeOpenDrtSnapshot(double peak = 100.0,
                         const QString& encoding_space = QStringLiteral("rec709"),
                         const QString& encoding_eotf = QStringLiteral("gamma_2_2"),
                         const QString& look_preset = QStringLiteral("standard"),
                         const QString& tonescale = QStringLiteral("use_look_preset"),
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

}  // namespace

// Panel loads via EditorAdjustmentStack and models are accessible.

TEST(EditorDisplayTransformSnapshotQmlTest, PanelLoadsAndAllModelsAreAccessible) {
  auto snapshot = MakeOpenDrtSnapshot();
  DisplayTransformSession session(snapshot, 0);
  AdjustmentStackHarness harness(&session);

  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  // All 8 display transform models must be found
  ASSERT_NE(harness.findModel<QObject>(QStringLiteral("displayMethodModel")), nullptr);
  ASSERT_NE(harness.findModel<QObject>(QStringLiteral("displayEncodingSpaceModel")), nullptr);
  ASSERT_NE(harness.findModel<QObject>(QStringLiteral("displayEncodingEotfModel")), nullptr);
  ASSERT_NE(harness.findModel<QObject>(QStringLiteral("displayPeakLuminanceModel")), nullptr);
  ASSERT_NE(harness.findModel<QObject>(QStringLiteral("displayAcesLimitingSpaceModel")), nullptr);
  ASSERT_NE(harness.findModel<QObject>(QStringLiteral("displayOpenDrtLookModel")), nullptr);
  ASSERT_NE(harness.findModel<QObject>(QStringLiteral("displayOpenDrtTonescaleModel")), nullptr);
  ASSERT_NE(harness.findModel<QObject>(QStringLiteral("displayOpenDrtCreativeWhiteModel")), nullptr);
}

TEST(EditorDisplayTransformSnapshotQmlTest, MethodModelDefaultIsOpenDrt) {
  auto snapshot = MakeOpenDrtSnapshot();
  DisplayTransformSession session(snapshot, 0);
  AdjustmentStackHarness harness(&session);

  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* methodModel = harness.findModel<QObject>(QStringLiteral("displayMethodModel"));
  ASSERT_NE(methodModel, nullptr);

  // defaultIndex is 1 (open_drt)
  EXPECT_EQ(methodModel->property("defaultIndex").toInt(), 1);

  // Entries have 2 values: aces_2_0, open_drt
  auto entries = methodModel->property("entries").toList();
  ASSERT_EQ(entries.size(), 2);
  EXPECT_EQ(entries[0].toMap().value("value").toString().toStdString(), "aces_2_0");
  EXPECT_EQ(entries[1].toMap().value("value").toString().toStdString(), "open_drt");
}

TEST(EditorDisplayTransformSnapshotQmlTest, EncodingSpaceHasSixEntries) {
  auto snapshot = MakeOpenDrtSnapshot();
  DisplayTransformSession session(snapshot, 0);
  AdjustmentStackHarness harness(&session);

  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* spaceModel = harness.findModel<QObject>(QStringLiteral("displayEncodingSpaceModel"));
  ASSERT_NE(spaceModel, nullptr);

  auto entries = spaceModel->property("entries").toList();
  ASSERT_EQ(entries.size(), 6);
  EXPECT_EQ(entries[0].toMap().value("value").toString().toStdString(), "rec709");
  EXPECT_EQ(entries[5].toMap().value("value").toString().toStdString(), "rec2020");
}

TEST(EditorDisplayTransformSnapshotQmlTest, PeakLuminanceRangeIsCorrect) {
  auto snapshot = MakeOpenDrtSnapshot();
  DisplayTransformSession session(snapshot, 0);
  AdjustmentStackHarness harness(&session);

  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* peakModel = harness.findModel<QObject>(QStringLiteral("displayPeakLuminanceModel"));
  ASSERT_NE(peakModel, nullptr);

  EXPECT_DOUBLE_EQ(peakModel->property("minimum").toDouble(), 100.0);
  EXPECT_DOUBLE_EQ(peakModel->property("maximum").toDouble(), 1000.0);
  EXPECT_DOUBLE_EQ(peakModel->property("defaultValue").toDouble(), 100.0);
  EXPECT_EQ(peakModel->property("suffix").toString().toStdString(), " nits");
}

TEST(EditorDisplayTransformSnapshotQmlTest, SubmitterIsWiredOnModel) {
  auto snapshot = MakeOpenDrtSnapshot();
  DisplayTransformSession session(snapshot, 0);
  AdjustmentStackHarness harness(&session);

  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* methodModel = harness.findModel<QObject>(QStringLiteral("displayMethodModel"));
  ASSERT_NE(methodModel, nullptr);

  auto* submitterObj = methodModel->property("submitter").value<QObject*>();
  ASSERT_NE(submitterObj, nullptr) << "submitter property not set on model";
  EXPECT_EQ(submitterObj, static_cast<QObject*>(&session));
}

TEST(EditorDisplayTransformSnapshotQmlTest, OpenDrtModelEntriesAreCorrect) {
  auto snapshot = MakeOpenDrtSnapshot();
  DisplayTransformSession session(snapshot, 0);
  AdjustmentStackHarness harness(&session);

  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* lookModel = harness.findModel<QObject>(QStringLiteral("displayOpenDrtLookModel"));
  ASSERT_NE(lookModel, nullptr);

  auto entries = lookModel->property("entries").toList();
  ASSERT_EQ(entries.size(), 8);
  EXPECT_EQ(entries[0].toMap().value("value").toString().toStdString(), "standard");
  EXPECT_EQ(entries[1].toMap().value("value").toString().toStdString(), "arriba");
}

TEST(EditorDisplayTransformSnapshotQmlTest, SubmitterFieldKeyIsOdt) {
  auto snapshot = MakeOpenDrtSnapshot();
  DisplayTransformSession session(snapshot, 0);
  AdjustmentStackHarness harness(&session);

  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* spaceModel = harness.findModel<QObject>(QStringLiteral("displayEncodingSpaceModel"));
  ASSERT_NE(spaceModel, nullptr);

  EXPECT_EQ(spaceModel->property("fieldKey").toString().toStdString(), "odt");
}

// Non-default ODT snapshot must project into every display model (load-only).
TEST(EditorDisplayTransformSnapshotQmlTest, LoadFromSnapshotProjectsNonDefaultOdtFields) {
  auto snapshot = MakeOpenDrtSnapshot(
      /*peak=*/420.0, QStringLiteral("rec2020"), QStringLiteral("st2084"),
      QStringLiteral("umbra"), QStringLiteral("aces_2_0"), QStringLiteral("d60"));
  DisplayTransformSession session(snapshot, 7);
  AdjustmentStackHarness harness(&session);

  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* methodModel = harness.findModel<QObject>(QStringLiteral("displayMethodModel"));
  auto* spaceModel  = harness.findModel<QObject>(QStringLiteral("displayEncodingSpaceModel"));
  auto* eotfModel   = harness.findModel<QObject>(QStringLiteral("displayEncodingEotfModel"));
  auto* peakModel   = harness.findModel<QObject>(QStringLiteral("displayPeakLuminanceModel"));
  auto* lookModel   = harness.findModel<QObject>(QStringLiteral("displayOpenDrtLookModel"));
  auto* toneModel   = harness.findModel<QObject>(QStringLiteral("displayOpenDrtTonescaleModel"));
  auto* whiteModel  = harness.findModel<QObject>(QStringLiteral("displayOpenDrtCreativeWhiteModel"));
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
  AdjustmentStackHarness harness(&session);
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
  AdjustmentStackHarness harness(&session);
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
  auto                   snapshot = MakeOpenDrtSnapshot();
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

}  // namespace alcedo::ui::test

#include "editor_display_transform_snapshot_qml_test.moc"
