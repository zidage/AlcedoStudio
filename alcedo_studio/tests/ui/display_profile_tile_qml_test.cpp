//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file display_profile_tile_qml_test.cpp
/// @brief W4 of the Windows display ICC plan: the Display profile tile of Settings > LUTs and
///        Color Management shows the destination of its window (section 3.2).
///
/// The offscreen platform has no monitor, so the test gives the WindowDisplayDestination of the
/// tile window monitor states through ApplyMonitorReading, the call that Refresh makes with the
/// Windows query. The profile files are real files written to a temporary directory.

#include <gtest/gtest.h>
#include <lcms2.h>

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "color/color_encoding_catalog.hpp"
#include "ui/alcedo_main/album_backend/display_profile_readout.hpp"
#include "ui/alcedo_main/app_theme.hpp"
#include "ui/edit_viewer/window_display_destination.hpp"

namespace alcedo::ui::test {
namespace {

namespace fs = std::filesystem;

void ProcessEvents(int milliseconds = 40) {
  QEventLoop loop;
  QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
  loop.exec();
}

auto QmlDirectoryUrl() -> QString {
  const auto path = fs::path(ALCEDO_TEST_SRC_DIR) / "ui" / "alcedo_main" / "qml";
  return QUrl::fromLocalFile(QString::fromStdString(path.string())).toString();
}

auto TileText(const char* source) -> QString {
  return QCoreApplication::translate("DisplayProfileTile", source);
}

/// The bytes of a P3-D65 gamma 2.2 display profile with the 'desc' text @p description.
auto P3ProfileBytes(const char* description) -> std::vector<std::byte> {
  const auto&           xy = color::GamutPrimariesXy(color::ColorGamutId::P3D65);
  const cmsCIExyY       white{xy[6], xy[7], 1.0};
  const cmsCIExyYTRIPLE primaries{{xy[0], xy[1], 1.0}, {xy[2], xy[3], 1.0}, {xy[4], xy[5], 1.0}};
  cmsToneCurve*         curve     = cmsBuildGamma(nullptr, 2.2);
  cmsToneCurve*         curves[3] = {curve, curve, curve};
  cmsHPROFILE           profile   = cmsCreateRGBProfile(&white, &primaries, curves);
  cmsFreeToneCurve(curve);
  cmsMLU* text = cmsMLUalloc(nullptr, 1);
  cmsMLUsetASCII(text, "en", "US", description);
  cmsWriteTag(profile, cmsSigProfileDescriptionTag, text);
  cmsMLUfree(text);
  cmsUInt32Number size = 0;
  EXPECT_TRUE(cmsSaveProfileToMem(profile, nullptr, &size));
  std::vector<std::byte> bytes(size);
  EXPECT_TRUE(cmsSaveProfileToMem(profile, bytes.data(), &size));
  cmsCloseProfile(profile);
  return bytes;
}

void WriteFile(const fs::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  ASSERT_TRUE(file.good());
}

auto Monitor(fs::path profile_path, bool advanced_color = false) -> MonitorColorProfile {
  MonitorColorProfile monitor;
  monitor.device_name           = L"\\\\.\\DISPLAY7";
  monitor.profile_path          = std::move(profile_path);
  monitor.advanced_color_active = advanced_color;
  return monitor;
}

auto NativePath(const fs::path& path) -> QString {
  return QDir::toNativeSeparators(QString::fromStdWString(path.wstring()));
}

const char* const kTileHarnessQml = R"QML(
import QtQuick
import "__QML_DIR__"

Window {
    width: 720
    height: 320
    visible: true

    DisplayProfileTile {
        width: 640
    }
}
)QML";

class DisplayProfileTileHarness {
 public:
  DisplayProfileTileHarness() {
    AppTheme::RegisterFonts();
    static const bool style_set = [] {
      QQuickStyle::setStyle(QStringLiteral("Basic"));
      return true;
    }();
    Q_UNUSED(style_set);
    RegisterDisplayProfileReadoutQmlType();
    QObject::connect(&engine_, &QQmlEngine::warnings, [this](const QList<QQmlError>& emitted) {
      for (const auto& warning : emitted) warnings_.push_back(warning.toString());
    });
    engine_.addImportPath(QStringLiteral("qrc:/"));
    engine_.addImportPath(QStringLiteral(ALCEDO_QT_QML_IMPORT_PATH));
    engine_.rootContext()->setContextProperty(QStringLiteral("appTheme"), &AppTheme::Instance());

    QByteArray qml{kTileHarnessQml};
    qml.replace("__QML_DIR__", QmlDirectoryUrl().toUtf8());
    engine_.loadData(qml, QUrl(QStringLiteral("file:///DisplayProfileTileHarness.qml")));
    if (!engine_.rootObjects().empty()) {
      window_ = qobject_cast<QQuickWindow*>(engine_.rootObjects().front());
    }
    if (window_ != nullptr) {
      // The first exposure and the activation refresh the destination (no monitor offscreen);
      // they run before the test gives its monitor states.
      EXPECT_TRUE(QTest::qWaitForWindowExposed(window_));
      ProcessEvents(80);
    }
  }

  auto Item(const char* name) -> QQuickItem* {
    return window_ != nullptr ? window_->findChild<QQuickItem*>(QString::fromLatin1(name))
                              : nullptr;
  }
  auto Text(const char* name) -> QString {
    QQuickItem* item = Item(name);
    return item != nullptr ? item->property("text").toString() : QString();
  }
  auto Visible(const char* name) -> bool {
    QQuickItem* item = Item(name);
    return item != nullptr && item->isVisible();
  }
  auto Readout() -> DisplayProfileReadout* {
    return window_ != nullptr ? window_->findChild<DisplayProfileReadout*>() : nullptr;
  }
  void Apply(const std::optional<MonitorColorProfile>& monitor) {
    WindowDisplayDestination::ForWindow(window_)->ApplyMonitorReading(monitor);
    ProcessEvents();
  }

  QQmlApplicationEngine engine_;
  QQuickWindow*         window_ = nullptr;
  QStringList           warnings_;
};

TEST(DisplayProfileTileQmlTest, ShowsDescriptionPathAndStateOfEachKind) {
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  const fs::path profile = fs::path(directory.path().toStdWString()) / "calibrated_p3.icc";
  const fs::path missing = fs::path(directory.path().toStdWString()) / "missing.icc";
  WriteFile(profile, P3ProfileBytes("Test P3 calibration"));

  DisplayProfileTileHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  ASSERT_NE(harness.Readout(), nullptr);
  EXPECT_EQ(harness.Readout()->window(), harness.window_);
  EXPECT_TRUE(harness.Visible("displayProfileTile"));

  // NoWindow: the offscreen window has no monitor.
  EXPECT_EQ(harness.Text("displayProfileDescription"), QString());
  EXPECT_EQ(harness.Text("displayProfileState"), TileText("No monitor"));
  EXPECT_FALSE(harness.Visible("displayProfilePath"));
  EXPECT_FALSE(harness.Visible("displayProfileError"));

  // MonitorProfile.
  harness.Apply(Monitor(profile));
  EXPECT_EQ(harness.Text("displayProfileDescription"), QStringLiteral("Test P3 calibration"));
  EXPECT_EQ(harness.Text("displayProfileState"), TileText("Applied"));
  EXPECT_EQ(harness.Text("displayProfilePath"), NativePath(profile));
  EXPECT_TRUE(harness.Visible("displayProfilePath"));
  EXPECT_FALSE(harness.Visible("displayProfileError"));

  // SrgbAdvancedColor (D9): the destination is sRGB, the monitor profile path stays.
  harness.Apply(Monitor(profile, /*advanced_color=*/true));
  EXPECT_EQ(harness.Text("displayProfileDescription"), QStringLiteral("sRGB"));
  EXPECT_EQ(harness.Text("displayProfileState"), TileText("Managed by Windows"));
  EXPECT_EQ(harness.Text("displayProfilePath"), NativePath(profile));
  EXPECT_FALSE(harness.Visible("displayProfileError"));

  // SrgbNoProfile (D10): no path.
  harness.Apply(Monitor({}));
  EXPECT_EQ(harness.Text("displayProfileDescription"), QStringLiteral("sRGB"));
  EXPECT_EQ(harness.Text("displayProfileState"), TileText("No profile, sRGB"));
  EXPECT_EQ(harness.Text("displayProfilePath"), QString());
  EXPECT_FALSE(harness.Visible("displayProfilePath"));
  EXPECT_FALSE(harness.Visible("displayProfileError"));

  // Unusable (D11): the reason, which names the file, is in the alert badge.
  harness.Apply(Monitor(missing));
  EXPECT_EQ(harness.Text("displayProfileDescription"), QString());
  EXPECT_EQ(harness.Text("displayProfileState"), TileText("Not applied"));
  EXPECT_EQ(harness.Text("displayProfilePath"), NativePath(missing));
  EXPECT_TRUE(harness.Visible("displayProfileError"));
  EXPECT_EQ(harness.Text("displayProfileError"), harness.Readout()->errorText());
  EXPECT_TRUE(harness.Text("displayProfileError").contains(QStringLiteral("missing.icc")))
      << harness.Text("displayProfileError").toStdString();

  // NoWindow again: the window loses its monitor.
  harness.Apply(std::nullopt);
  EXPECT_EQ(harness.Text("displayProfileDescription"), QString());
  EXPECT_EQ(harness.Text("displayProfileState"), TileText("No monitor"));
  EXPECT_FALSE(harness.Visible("displayProfilePath"));
  EXPECT_FALSE(harness.Visible("displayProfileError"));
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

TEST(DisplayProfileTileQmlTest, DestinationChangeUpdatesTheTexts) {
  QTemporaryDir directory;
  ASSERT_TRUE(directory.isValid());
  const fs::path profile = fs::path(directory.path().toStdWString()) / "monitor.icc";
  WriteFile(profile, P3ProfileBytes("First calibration"));

  DisplayProfileTileHarness harness;
  ASSERT_NE(harness.window_, nullptr) << harness.warnings_.join('\n').toStdString();
  ASSERT_NE(harness.Readout(), nullptr);
  QSignalSpy changes(harness.Readout(), &DisplayProfileReadout::destinationChanged);

  harness.Apply(Monitor(profile));
  EXPECT_EQ(changes.count(), 1);
  EXPECT_EQ(harness.Text("displayProfileDescription"), QStringLiteral("First calibration"));
  EXPECT_EQ(harness.Text("displayProfileState"), TileText("Applied"));

  // An equal monitor state publishes nothing, and the texts stay.
  harness.Apply(Monitor(profile));
  EXPECT_EQ(changes.count(), 1);
  EXPECT_EQ(harness.Text("displayProfileDescription"), QStringLiteral("First calibration"));

  // A new calibration at the same path is a new destination.
  WriteFile(profile, P3ProfileBytes("Second calibration after the monitor warmed up"));
  harness.Apply(Monitor(profile));
  EXPECT_EQ(changes.count(), 2);
  EXPECT_EQ(harness.Text("displayProfileDescription"),
            QStringLiteral("Second calibration after the monitor warmed up"));
  EXPECT_EQ(harness.Text("displayProfileState"), TileText("Applied"));
  EXPECT_EQ(harness.Text("displayProfilePath"), NativePath(profile));

  // Windows Advanced Color turned on.
  harness.Apply(Monitor(profile, /*advanced_color=*/true));
  EXPECT_EQ(changes.count(), 3);
  EXPECT_EQ(harness.Text("displayProfileDescription"), QStringLiteral("sRGB"));
  EXPECT_EQ(harness.Text("displayProfileState"), TileText("Managed by Windows"));
  EXPECT_TRUE(harness.warnings_.isEmpty()) << harness.warnings_.join('\n').toStdString();
}

}  // namespace
}  // namespace alcedo::ui::test
