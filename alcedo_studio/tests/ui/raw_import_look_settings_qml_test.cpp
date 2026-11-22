//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// Settings > Default Behavior > RAW Import (#920) on the production SettingDialog and
// ApplicationModuleHost. QSettings goes to a temporary INI directory. The values are edited
// through the section model (appModules.rawImportLook); offscreen pointer delivery into the
// scrolled page of the dialog is not reliable (AGENTS.md). The test checks that the rows follow
// the model, the sliders clamp to the hard range and reset on a double-click (resetRequested),
// the OpenDRT Look row follows the method, the preview takes no input, and Done writes
// the look that RawImportLookSettings::Read returns.

#include <gtest/gtest.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <filesystem>
#include <memory>

#include "app/raw_import_look_settings.hpp"
#include "edit/graph/drt_node_model.hpp"
#include "edit/graph/pipeline_document.hpp"
#include "ui/alcedo_main/album_backend/application_module_host.hpp"
#include "ui/alcedo_main/album_backend/raw_import_look_settings_model.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/alcedo_main/language_manager.hpp"
#include "ui/alcedo_main/shortcut_registry.hpp"

namespace alcedo::ui::test {
namespace {

void ProcessEvents(int milliseconds = 40) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

auto QmlDirectoryUrl() -> QString {
  const auto path = std::filesystem::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml";
  return QUrl::fromLocalFile(QString::fromStdString(path.string())).toString();
}

const char* const kDialogHarnessQml        = R"QML(
import QtQuick
import QtQuick.Controls
import "__QML_DIR__"

ApplicationWindow {
    width: 1280
    height: 800
    visible: true

    SettingDialog {
        objectName: "settingsDialog"
        languageOptions: languageManager.availableLanguages
    }
}
)QML";

constexpr int     kDefaultBehaviorCategory = 1;

class RawImportLookSettingsQmlTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(settings_dir_.isValid());
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings_dir_.path());
    QCoreApplication::setOrganizationName(QStringLiteral("AlcedoTests"));
    QCoreApplication::setApplicationName(QStringLiteral("RawImportLookSettingsHarness"));

    AppTheme::RegisterFonts();
    // The style can be set once per process, before the first Controls import.
    static const bool style_set = [] {
      QQuickStyle::setStyle(QStringLiteral("Basic"));
      return true;
    }();
    Q_UNUSED(style_set);
    host_             = std::make_unique<ApplicationModuleHost>();
    language_manager_ = std::make_unique<LanguageManager>(QCoreApplication::instance());
    AppTheme::SetEffectiveLanguageCode(language_manager_->EffectiveLanguageCode());
    RegisterShortcutRegistryQmlType();
    engine_ = std::make_unique<QQmlApplicationEngine>();
    QObject::connect(engine_.get(), &QQmlEngine::warnings, [this](const QList<QQmlError>& emitted) {
      for (const auto& warning : emitted) warnings_.push_back(warning.toString());
    });
    engine_->addImportPath(QStringLiteral("qrc:/"));
    engine_->addImportPath(QStringLiteral(ALCEDO_QT_QML_IMPORT_PATH));
    engine_->rootContext()->setContextProperty(QStringLiteral("appModules"), host_.get());
    engine_->rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());
    engine_->rootContext()->setContextProperty(QStringLiteral("languageManager"),
                                               language_manager_.get());
    host_->AttachQmlEngine(engine_.get());

    QByteArray qml{kDialogHarnessQml};
    qml.replace("__QML_DIR__", QmlDirectoryUrl().toUtf8());
    engine_->loadData(qml, QUrl(QStringLiteral("file:///RawImportLookSettingsHarness.qml")));
    if (!engine_->rootObjects().empty()) {
      window_ = qobject_cast<QQuickWindow*>(engine_->rootObjects().front());
      if (window_ != nullptr) window_->show();
    }
    ProcessEvents(120);
  }

  void TearDown() override {
    engine_.reset();
    host_.reset();
  }

  auto dialog() -> QObject* {
    return window_ != nullptr ? window_->findChild<QObject*>(QStringLiteral("settingsDialog"))
                              : nullptr;
  }
  auto item(const char* name) -> QQuickItem* {
    return window_ != nullptr ? window_->findChild<QQuickItem*>(QString::fromLatin1(name))
                              : nullptr;
  }
  auto model() -> RawImportLookSettingsModel* { return host_->raw_import_look(); }

  void Open() {
    dialog()->setProperty("requestedCategory", kDefaultBehaviorCategory);
    QMetaObject::invokeMethod(dialog(), "open");
    ProcessEvents(150);
  }

  QTemporaryDir                          settings_dir_;
  std::unique_ptr<ApplicationModuleHost> host_;
  std::unique_ptr<LanguageManager>       language_manager_;
  std::unique_ptr<QQmlApplicationEngine> engine_;
  QQuickWindow*                          window_ = nullptr;
  QStringList                            warnings_;
};

TEST_F(RawImportLookSettingsQmlTest, RowsFollowTheModelAndDoneWritesTheLook) {
  ASSERT_NE(dialog(), nullptr) << warnings_.join('\n').toStdString();
  // A value stored before the dialog opens shows on the page.
  RawImportLookSettings().SetExposureEv(2.0);
  Open();
  ASSERT_NE(item("rawImportLookSettingsSection"), nullptr) << warnings_.join('\n').toStdString();
  EXPECT_DOUBLE_EQ(model()->exposure_ev(), 2.0);

  // The rows have no reset buttons; the value of a changed slider shows in the text color.
  EXPECT_EQ(item("rawImportExposureRowReset"), nullptr);
  auto* exposure_slider   = item("rawImportExposureSlider");
  auto* saturation_slider = item("rawImportSaturationSlider");
  ASSERT_NE(exposure_slider, nullptr);
  ASSERT_NE(saturation_slider, nullptr);
  EXPECT_DOUBLE_EQ(exposure_slider->property("value").toDouble(), 2.0);
  EXPECT_TRUE(exposure_slider->property("modified").toBool());
  EXPECT_FALSE(saturation_slider->property("modified").toBool());
  EXPECT_EQ(item("rawImportExposureSliderValueText")->property("text").toString(), "+2.00");

  // An edit outside the hard range is clamped to it; the rail covers the hard range, and the
  // handle at +10 EV stays inside the rail at its end.
  QMetaObject::invokeMethod(exposure_slider, "edit", Q_ARG(QVariant, 12.0));
  ProcessEvents(200);
  EXPECT_DOUBLE_EQ(model()->exposure_ev(), 10.0);
  EXPECT_TRUE(exposure_slider->property("beyondSoftRange").toBool());
  auto* rail   = item("rawImportExposureSliderRail");
  auto* handle = item("rawImportExposureSliderHandle");
  ASSERT_NE(rail, nullptr);
  ASSERT_NE(handle, nullptr);
  EXPECT_DOUBLE_EQ(handle->x() + handle->width(), rail->width() - 2.0);
  QVariant parsed;
  QMetaObject::invokeMethod(exposure_slider, "parseText", Q_RETURN_ARG(QVariant, parsed),
                            Q_ARG(QVariant, QString(QChar(0x2212)) + QStringLiteral("3.5")));
  EXPECT_DOUBLE_EQ(parsed.toDouble(), -3.5);

  // A double-click on the rail or the row name resets the row to the app default.
  QMetaObject::invokeMethod(saturation_slider, "edit", Q_ARG(QVariant, 55.0));
  ProcessEvents();
  EXPECT_DOUBLE_EQ(model()->saturation(), 55.0);
  QMetaObject::invokeMethod(saturation_slider, "resetRequested");
  ProcessEvents();
  EXPECT_DOUBLE_EQ(model()->saturation(), 30.0);
  ASSERT_TRUE(model()->setToneCurvePreset(QStringLiteral("linear")));
  QMetaObject::invokeMethod(item("rawImportToneCurveRow"), "resetRequested");
  ProcessEvents();
  EXPECT_EQ(model()->tone_curve_preset(), QStringLiteral("cl_digital"));

  auto* tone_curve_combo = item("rawImportToneCurveCombo");
  ASSERT_NE(tone_curve_combo, nullptr);
  EXPECT_EQ(tone_curve_combo->property("currentIndex").toInt(), 4);  // Cl. Digit.

  // The preview is display only and draws the line of the selected preset.
  auto* plot_pane  = item("rawImportToneCurvePlotPane");
  auto* curve_line = window_->findChild<QObject*>(QStringLiteral("rawImportToneCurveLine"));
  ASSERT_NE(plot_pane, nullptr);
  ASSERT_NE(curve_line, nullptr);
  EXPECT_FALSE(plot_pane->isEnabled());
  EXPECT_EQ(curve_line->property("points").value<QList<QPointF>>(), model()->tone_curve_line());
  ASSERT_TRUE(model()->setToneCurvePreset(QStringLiteral("strong_contrast")));
  ProcessEvents();
  EXPECT_EQ(tone_curve_combo->property("currentIndex").toInt(), 3);
  EXPECT_EQ(curve_line->property("points").value<QList<QPointF>>(), model()->tone_curve_line());

  // The OpenDRT Look row shows only while OpenDRT is selected.
  auto* open_drt_look_row = item("rawImportOpenDrtLookRow");
  ASSERT_NE(open_drt_look_row, nullptr);
  EXPECT_TRUE(open_drt_look_row->isVisible());
  ASSERT_TRUE(model()->setOpenDrtLookPreset(QStringLiteral("arriba")));
  ASSERT_TRUE(model()->setDrtMethod(QStringLiteral("aces_2_0")));
  ProcessEvents();
  EXPECT_FALSE(open_drt_look_row->isVisible());

  QMetaObject::invokeMethod(item("rawImportExposureRow"), "resetRequested");
  ProcessEvents();
  EXPECT_DOUBLE_EQ(model()->exposure_ev(), 1.5);
  EXPECT_FALSE(exposure_slider->property("modified").toBool());

  QMetaObject::invokeMethod(dialog(), "applySettings");
  ProcessEvents(150);

  const auto look = RawImportLookSettings().Read();
  EXPECT_FLOAT_EQ(look.exposure_ev, 1.5f);
  EXPECT_FALSE(QSettings().contains(RawImportLookSettings::Key(RawImportLookField::ExposureEv)));
  EXPECT_EQ(look.tone_curve_preset_id, "strong_contrast");
  EXPECT_EQ(look.drt_method, DrtMethod::Aces20);
  EXPECT_EQ(look.open_drt_look_preset, "arriba");
  EXPECT_TRUE(warnings_.isEmpty()) << warnings_.join('\n').toStdString();
}

}  // namespace
}  // namespace alcedo::ui::test
