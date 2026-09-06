//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file editor_adjustment_snapshot_qml_test.cpp
/// @brief Verifies that an already-restored editor snapshot populates Tone controls on first bind.

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
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_submitter.hpp"
#include "ui/alcedo_main/album_backend/editor_panel_presentation.hpp"
#include "app/editor_panel_projection.hpp"
#include "ui/alcedo_main/app_theme.hpp"

namespace alcedo::ui::test {
namespace {

class SnapshotSession final : public QObject, public IEditorAdjustmentSubmitter {
  Q_OBJECT
  Q_PROPERTY(QVariantMap adjustmentSnapshot READ adjustmentSnapshot NOTIFY AdjustmentSnapshotChanged)
  Q_PROPERTY(quint64 snapshotRevision READ snapshotRevision NOTIFY AdjustmentSnapshotChanged)

 public:
  explicit SnapshotSession(QVariantMap snapshot, quint64 revision)
      : snapshot_(std::move(snapshot)), revision_(revision) {}

  auto adjustmentSnapshot() const -> QVariantMap { return snapshot_; }
  auto snapshotRevision() const -> quint64 { return revision_; }
    auto submitWrite(QString fieldKey, alcedo::EditorParameterWrite write, bool settled)
        -> bool override {
      static_cast<void>(fieldKey);
      static_cast<void>(write);
      static_cast<void>(settled);
      ++submit_count_;
      return true;
    }

  auto submitPatch(QString fieldKey, QString paramsJson, bool settled) -> bool override {
    static_cast<void>(fieldKey);
    static_cast<void>(paramsJson);
    static_cast<void>(settled);
    ++submit_count_;
    return true;
  }
  auto canEdit() const -> bool override { return true; }
  auto submitCount() const -> int { return submit_count_; }

 signals:
  void AdjustmentSnapshotChanged();

 private:
  QVariantMap snapshot_;
  quint64     revision_     = 0;
  int         submit_count_ = 0;
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

class AdjustmentSnapshotQmlHarness {
 public:
  explicit AdjustmentSnapshotQmlHarness(SnapshotSession* session) {
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
    window_.resize(500, 700);
    window_.show();
    QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
  }

  auto root() const -> QQuickItem* { return root_.get(); }
  auto errors() const -> QString {
    QStringList text;
    for (const auto& error : errors_) {
      text.push_back(error.toString());
    }
    return text.join('\n');
  }

 private:
  QQmlEngine                  engine_;
  QQuickWindow                window_;
  std::unique_ptr<QQuickItem> root_;
  QList<QQmlError>            errors_;
};

TEST(EditorAdjustmentSnapshotQmlTest, ExistingSnapshotIsAppliedOnFirstEditorBinding) {
  QVariantMap snapshot;
  snapshot.insert(QStringLiteral("exposure"),
                  QVariantMap{{QStringLiteral("exposure"), -2.25}});
  snapshot.insert(QStringLiteral("contrast"),
                  QVariantMap{{QStringLiteral("contrast"), 27.0}});
  SnapshotSession session(std::move(snapshot), 0);
  AdjustmentSnapshotQmlHarness harness(&session);

  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();
  auto* exposure = harness.root()->findChild<QObject*>(QStringLiteral("toneExposureModel"));
  auto* contrast = harness.root()->findChild<QObject*>(QStringLiteral("toneContrastModel"));
  ASSERT_NE(exposure, nullptr);
  ASSERT_NE(contrast, nullptr);

  EXPECT_DOUBLE_EQ(exposure->property("value").toDouble(), -2.25);
  EXPECT_DOUBLE_EQ(contrast->property("value").toDouble(), 27.0);
  EXPECT_EQ(session.submitCount(), 0);
}

TEST(EditorAdjustmentSnapshotQmlTest, QmlLoadFromTypedProjectionDoesNotSubmit) {
  alcedo::EditorPanelProjection projection;
  projection.session_generation = 1;
  alcedo::EditorPanelFieldPresentation exposure;
  exposure.field_key = "exposure";
  exposure.value     = alcedo::EditorPanelScalarValue{"exposure", -1.5f};
  alcedo::EditorPanelFieldPresentation saturation;
  saturation.field_key = "saturation";
  saturation.value     = alcedo::EditorPanelScalarValue{"saturation", 40.0f};
  alcedo::EditorPanelFieldPresentation lut;
  lut.field_key = "lut";
  lut.value     = alcedo::EditorPanelLutValue{"D:/luts/look.cube"};
  projection.fields.push_back(std::move(exposure));
  projection.fields.push_back(std::move(saturation));
  projection.fields.push_back(std::move(lut));

  SnapshotSession session(alcedo::ui::PanelProjectionToVariantMap(projection), 1);
  AdjustmentSnapshotQmlHarness harness(&session);
  ASSERT_NE(harness.root(), nullptr) << harness.errors().toStdString();

  auto* tone_exposure = harness.root()->findChild<QObject*>(QStringLiteral("toneExposureModel"));
  auto* look_saturation =
      harness.root()->findChild<QObject*>(QStringLiteral("lookSaturationModel"));
  auto* lut_model = harness.root()->findChild<QObject*>(QStringLiteral("adjustmentStackLutModel"));
  ASSERT_NE(tone_exposure, nullptr);
  ASSERT_NE(look_saturation, nullptr);
  ASSERT_NE(lut_model, nullptr);
  EXPECT_DOUBLE_EQ(tone_exposure->property("value").toDouble(), -1.5);
  EXPECT_DOUBLE_EQ(look_saturation->property("value").toDouble(), 40.0);
  EXPECT_EQ(lut_model->property("selectedPath").toString(), QStringLiteral("D:/luts/look.cube"));
  EXPECT_EQ(session.submitCount(), 0);
}

}  // namespace
}  // namespace alcedo::ui::test

#include "editor_adjustment_snapshot_qml_test.moc"
