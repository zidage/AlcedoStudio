//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Qt Quick Test for the RAW Decode lens-calibration picker.
//
// tst_lens_catalog_picker.qml drives LensCatalogPicker standalone against an
// inline fixture catalog (fuzzy matching, brand filter, detection mapping).
// tst_lens_auto_detect.qml hosts the production EditorRawDecodePanel against
// LensPanelSession — a recording IEditorAdjustmentSubmitter with a scripted
// EXIF lens identity — and verifies the auto-recognition checkbox behavior:
// default checked when detection succeeds, disabled + "需手动选择" label when
// it fails, manual picks uncheck it, re-checking restores the detected row.
//
// Build (win_debug):
//   cmd /c scripts\msvc_env.cmd --build --preset win_debug --target
//   EditorLensPickerQuickTest
// Run:
//   build\debug\...\EditorLensPickerQuickTest.exe -o -,txt -v1

#include <QApplication>
#include <QCoreApplication>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickStyle>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>
#include <QtQuickTest>
#include <filesystem>

#include "json.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_models.hpp"
#include "ui/alcedo_main/album_backend/editor_adjustment_submitter.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/qt_test_plugin_paths.hpp"

namespace alcedo::ui::quicktest {
namespace {

auto SrcQmlDir() -> QString {
  return QString::fromStdString(
      (std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml").string());
}

/// Recording submitter + scripted EXIF lens identity. The QML panel only
/// requires adjustmentSnapshot, exifLensMake/exifLensModel, and the
/// IEditorAdjustmentSubmitter interface; calls are exposed as plain maps so
/// the QML tests can JSON.parse paramsJson directly.
class LensPanelSession final : public QObject, public IEditorAdjustmentSubmitter {
  Q_OBJECT
  Q_PROPERTY(
      QVariantMap adjustmentSnapshot READ adjustmentSnapshot NOTIFY adjustmentSnapshotChanged)
  Q_PROPERTY(QString exifLensMake READ exifLensMake NOTIFY imageExifChanged)
  Q_PROPERTY(QString exifLensModel READ exifLensModel NOTIFY imageExifChanged)
  Q_PROPERTY(QVariantList calls READ calls NOTIFY callsChanged)

 public:
  explicit LensPanelSession(QObject* parent = nullptr) : QObject(parent) {}

  [[nodiscard]] auto adjustmentSnapshot() const -> QVariantMap { return snapshot_; }
  [[nodiscard]] auto exifLensMake() const -> QString { return exif_make_; }
  [[nodiscard]] auto exifLensModel() const -> QString { return exif_model_; }
  [[nodiscard]] auto calls() const -> QVariantList { return calls_; }

  [[nodiscard]] auto canEdit() const -> bool override { return true; }

  auto               submitWrite(QString fieldKey, alcedo::EditorParameterWrite write, bool settled)
      -> bool override {
    calls_.push_back(MakeCall(fieldKey, QString(), settled, write));
    emit callsChanged();
    return true;
  }

  // Q_INVOKABLE matches EditorSessionController: the panel's raw-JSON submit
  // path calls submitPatch through the QML metaobject, not the C++ vtable.
  Q_INVOKABLE bool submitPatch(QString fieldKey, QString paramsJson, bool settled) override {
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
    calls_.push_back(MakeCall(fieldKey, paramsJson, settled, *write));
    emit callsChanged();
    return true;
  }

  Q_INVOKABLE void setExifLens(const QString& make, const QString& model) {
    if (exif_make_ == make && exif_model_ == model) {
      return;
    }
    exif_make_  = make;
    exif_model_ = model;
    emit imageExifChanged();
  }

  Q_INVOKABLE void setSnapshot(const QVariantMap& snapshot) {
    snapshot_ = snapshot;
    emit adjustmentSnapshotChanged();
  }

  Q_INVOKABLE void clearCalls() {
    calls_.clear();
    emit callsChanged();
  }

  Q_INVOKABLE QVariantMap callAt(int index) const {
    if (index < 0 || index >= calls_.size()) {
      return {};
    }
    return calls_.at(index).toMap();
  }

 signals:
  void adjustmentSnapshotChanged();
  void imageExifChanged();
  void callsChanged();

 private:
  /// Each recorded call carries a decoded `lens` map (enabled/lens_maker/
  /// lens_model) so QML assertions work uniformly for both the typed
  /// submitWrite path (models) and the raw JSON submitPatch path (panel).
  static auto MakeCall(const QString& fieldKey, const QString& paramsJson, bool settled,
                       const alcedo::EditorParameterWrite& write) -> QVariantMap {
    QVariantMap call;
    call.insert(QStringLiteral("fieldKey"), fieldKey);
    call.insert(QStringLiteral("paramsJson"), paramsJson);
    call.insert(QStringLiteral("settled"), settled);
    if (const auto* lens = std::get_if<alcedo::DevelopLensCalibrationUpdate>(&write)) {
      QVariantMap lens_map;
      if (lens->lens_enabled.has_value()) {
        lens_map.insert(QStringLiteral("enabled"), *lens->lens_enabled);
      }
      if (lens->lens_maker.has_value()) {
        lens_map.insert(QStringLiteral("lens_maker"), QString::fromStdString(*lens->lens_maker));
      }
      if (lens->lens_model.has_value()) {
        lens_map.insert(QStringLiteral("lens_model"), QString::fromStdString(*lens->lens_model));
      }
      call.insert(QStringLiteral("lens"), lens_map);
    }
    return call;
  }

  QVariantMap  snapshot_;
  QVariantList calls_;
  QString      exif_make_;
  QString      exif_model_;
};

class LensPickerSetup : public QObject {
  Q_OBJECT

 public:
  LensPickerSetup() {
    RegisterEditorAdjustmentQmlTypes();
    AppTheme::RegisterFonts();
    AppTheme::Instance().setReduceMotion(true);
    session_ = new LensPanelSession(this);
  }

 public slots:
  void applicationAvailable() { QQuickStyle::setStyle(QStringLiteral("Basic")); }

  void qmlEngineAvailable(QQmlEngine* engine) {
    engine->addImportPath(QStringLiteral("qrc:/"));
    // The QuickTest engine does not inherit QLibraryInfo qml paths in this
    // isolated runtime dir; stamp the real Qt qml dir for QtQuick.Controls.
    engine->addImportPath(QStringLiteral(ALCEDO_QT_QML_IMPORT_PATH));
    engine->addImportPath(SrcQmlDir());
    engine->rootContext()->setContextProperty(
        QStringLiteral("appTheme"),
        QVariant::fromValue(static_cast<QObject*>(&AppTheme::Instance())));
    engine->rootContext()->setContextProperty(QStringLiteral("lensSession"), session_);
    engine->rootContext()->setContextProperty(
        QStringLiteral("lensPickerUrl"),
        QUrl::fromLocalFile(SrcQmlDir() + QStringLiteral("/LensCatalogPicker.qml")));
    engine->rootContext()->setContextProperty(
        QStringLiteral("lensPanelUrl"),
        QUrl::fromLocalFile(SrcQmlDir() + QStringLiteral("/EditorRawDecodePanel.qml")));
  }

 private:
  LensPanelSession* session_ = nullptr;
};

}  // namespace
}  // namespace alcedo::ui::quicktest

int main(int argc, char** argv) {
  qputenv("QML_DISABLE_DISK_CACHE", QByteArray("1"));
  // Stamps QT_PLUGIN_PATH (imageformats → qsvg for ColorImage SVG icons) and
  // QML2_IMPORT_PATH before QApplication — the isolated runtime dir only
  // carries applocal DLLs.
  alcedo::ui::test::ConfigureQtPluginPaths(argv[0]);
  QApplication app(argc, argv);
  QQuickStyle::setStyle(QStringLiteral("Basic"));
  alcedo::ui::quicktest::LensPickerSetup setup;
  return quick_test_main_with_setup(argc, argv, "EditorLensPickerQuickTest", QUICK_TEST_SOURCE_DIR,
                                    &setup);
}

#include "editor_lens_picker_quicktest.moc"
