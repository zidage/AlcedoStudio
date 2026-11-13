//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file window_display_destination_test.cpp
/// @brief W2 of the Windows display ICC plan: the destination rules D9, D10, and D11, the change
///        detection of the monitor state, and the window object on the Qt offscreen platform.

#include <gtest/gtest.h>
#include <lcms2.h>

#include <QCoreApplication>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QWindow>
#include <QtGlobal>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "color/color_encoding_catalog.hpp"
#include "ui/edit_viewer/display_color_transform.hpp"
#include "ui/edit_viewer/window_display_destination.hpp"

namespace alcedo {
namespace {

constexpr const char* kProfileDescription = "Test P3 calibration";

auto ToPath(const QString& path) -> std::filesystem::path {
  return std::filesystem::path(path.toStdWString());
}

auto PathText(const std::filesystem::path& path) -> std::string {
  const auto text = path.u8string();
  return std::string(text.begin(), text.end());
}

/// The bytes of a P3-D65 gamma 2.2 display profile with the 'desc' text kProfileDescription and
/// the gamma @p gamma.
auto P3ProfileBytes(double gamma = 2.2) -> std::vector<std::byte> {
  const auto&           xy = color::GamutPrimariesXy(color::ColorGamutId::P3D65);
  const cmsCIExyY       white{xy[6], xy[7], 1.0};
  const cmsCIExyYTRIPLE primaries{{xy[0], xy[1], 1.0}, {xy[2], xy[3], 1.0}, {xy[4], xy[5], 1.0}};
  cmsToneCurve*         curve     = cmsBuildGamma(nullptr, gamma);
  cmsToneCurve*         curves[3] = {curve, curve, curve};
  cmsHPROFILE           profile   = cmsCreateRGBProfile(&white, &primaries, curves);
  cmsFreeToneCurve(curve);
  cmsMLU* description = cmsMLUalloc(nullptr, 1);
  cmsMLUsetASCII(description, "en", "US", kProfileDescription);
  cmsWriteTag(profile, cmsSigProfileDescriptionTag, description);
  cmsMLUfree(description);
  cmsUInt32Number size = 0;
  EXPECT_TRUE(cmsSaveProfileToMem(profile, nullptr, &size));
  std::vector<std::byte> bytes(size);
  EXPECT_TRUE(cmsSaveProfileToMem(profile, bytes.data(), &size));
  cmsCloseProfile(profile);
  return bytes;
}

void WriteFile(const std::filesystem::path& path, const std::vector<std::byte>& bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  ASSERT_TRUE(file.good());
}

auto Monitor(std::filesystem::path profile_path, bool advanced_color = false)
    -> MonitorColorProfile {
  MonitorColorProfile monitor;
  monitor.device_name           = L"\\\\.\\DISPLAY7";
  monitor.profile_path          = std::move(profile_path);
  monitor.advanced_color_active = advanced_color;
  return monitor;
}

class WindowDisplayDestinationTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(dir_.isValid()); }

  auto FilePath(const char* name) const -> std::filesystem::path {
    return ToPath(dir_.filePath(QString::fromUtf8(name)));
  }

  QTemporaryDir dir_;
};

TEST_F(WindowDisplayDestinationTest, AdvancedColorMonitorTargetsSrgb) {
  const auto path = FilePath("calibrated.icc");
  WriteFile(path, P3ProfileBytes());
  const auto destination = ResolveDisplayDestination(Monitor(path, /*advanced_color=*/true));
  EXPECT_EQ(destination.kind, DisplayDestinationKind::SrgbAdvancedColor);
  EXPECT_EQ(destination.profile_bytes, SrgbDisplayProfile());
  EXPECT_EQ(destination.description, "sRGB");
  // The tile still names the monitor profile that Windows converts to.
  EXPECT_EQ(destination.profile_path, path);
  EXPECT_TRUE(destination.error.empty());
}

TEST_F(WindowDisplayDestinationTest, MonitorWithoutProfileTargetsSrgb) {
  const auto destination = ResolveDisplayDestination(Monitor({}));
  EXPECT_EQ(destination.kind, DisplayDestinationKind::SrgbNoProfile);
  EXPECT_EQ(destination.profile_bytes, SrgbDisplayProfile());
  EXPECT_EQ(destination.description, "sRGB");
  EXPECT_TRUE(destination.profile_path.empty());
}

TEST_F(WindowDisplayDestinationTest, UnreadableProfileIsUnusableWithReason) {
  const auto missing = FilePath("missing.icc");
  const auto text    = FilePath("notes.icc");
  {
    std::ofstream file(text);
    file << "This file is a text file and no ICC profile.\n";
  }
  for (const auto& path : {missing, text}) {
    const auto destination = ResolveDisplayDestination(Monitor(path));
    EXPECT_EQ(destination.kind, DisplayDestinationKind::Unusable) << PathText(path);
    EXPECT_TRUE(destination.profile_bytes.empty());
    EXPECT_EQ(destination.profile_path, path);
    EXPECT_NE(destination.error.find(PathText(path)), std::string::npos) << destination.error;
  }
  EXPECT_NE(ResolveDisplayDestination(Monitor(missing)).error.find("cannot read"),
            std::string::npos);
  EXPECT_NE(ResolveDisplayDestination(Monitor(text)).error.find("cannot be parsed"),
            std::string::npos);
}

TEST_F(WindowDisplayDestinationTest, ValidProfileIsTheDestination) {
  const auto path  = FilePath("calibrated.icc");
  const auto bytes = P3ProfileBytes();
  WriteFile(path, bytes);
  const auto destination = ResolveDisplayDestination(Monitor(path));
  EXPECT_EQ(destination.kind, DisplayDestinationKind::MonitorProfile);
  EXPECT_EQ(destination.profile_bytes, bytes);
  EXPECT_EQ(destination.description, kProfileDescription);
  EXPECT_EQ(destination.profile_path, path);
  EXPECT_TRUE(destination.error.empty());
}

TEST_F(WindowDisplayDestinationTest, EqualMonitorStatePublishesNothing) {
  const auto path = FilePath("calibrated.icc");
  WriteFile(path, P3ProfileBytes(2.2));
  DisplayDestinationTracker tracker;
  ASSERT_NE(tracker.Destination(), nullptr);
  EXPECT_EQ(tracker.Destination()->kind, DisplayDestinationKind::NoWindow);
  EXPECT_FALSE(tracker.Update(std::nullopt)) << "no window before and after";

  EXPECT_TRUE(tracker.Update(Monitor(path)));
  const auto first = tracker.Destination();
  EXPECT_EQ(first->kind, DisplayDestinationKind::MonitorProfile);
  EXPECT_FALSE(tracker.Update(Monitor(path)));
  EXPECT_EQ(tracker.Destination(), first) << "an equal state keeps the published value";

  // A new calibration at the same path: a different size and write time.
  auto recalibrated = P3ProfileBytes(2.4);
  recalibrated.resize(recalibrated.size() + 4, std::byte{0});
  WriteFile(path, recalibrated);
  std::filesystem::last_write_time(path, std::filesystem::last_write_time(path) +
                                             std::chrono::seconds(2));
  EXPECT_TRUE(tracker.Update(Monitor(path)));
  EXPECT_NE(tracker.Destination(), first);
  EXPECT_EQ(tracker.Destination()->profile_bytes, recalibrated);

  EXPECT_TRUE(tracker.Update(Monitor(path, /*advanced_color=*/true)));
  EXPECT_EQ(tracker.Destination()->kind, DisplayDestinationKind::SrgbAdvancedColor);
  auto other_monitor        = Monitor(path, /*advanced_color=*/true);
  other_monitor.device_name = L"\\\\.\\DISPLAY8";
  EXPECT_TRUE(tracker.Update(other_monitor));
  EXPECT_TRUE(tracker.Update(std::nullopt));
  EXPECT_EQ(tracker.Destination()->kind, DisplayDestinationKind::NoWindow);
}

std::atomic<int> g_warning_count{0};

void CountWarnings(QtMsgType type, const QMessageLogContext& /*context*/,
                   const QString& /*message*/) {
  if (type == QtWarningMsg || type == QtCriticalMsg) {
    ++g_warning_count;
  }
}

TEST_F(WindowDisplayDestinationTest, OffscreenWindowHasNoDestination) {
  ASSERT_EQ(QGuiApplication::platformName(), QStringLiteral("offscreen"));
  g_warning_count                     = 0;
  const QtMessageHandler previous     = qInstallMessageHandler(CountWarnings);

  QWindow window;
  window.resize(64, 48);
  window.show();
  auto* destination = WindowDisplayDestination::ForWindow(&window);
  ASSERT_NE(destination, nullptr);
  EXPECT_EQ(WindowDisplayDestination::ForWindow(&window), destination)
      << "one destination object for each window";
  EXPECT_EQ(destination->parent(), &window);
  QSignalSpy changes(destination, &WindowDisplayDestination::DestinationChanged);
  destination->Refresh();
  QCoreApplication::processEvents();
  ASSERT_NE(destination->Destination(), nullptr);
  EXPECT_EQ(destination->Destination()->kind, DisplayDestinationKind::NoWindow);
  EXPECT_TRUE(destination->Destination()->profile_bytes.empty());
  EXPECT_EQ(changes.count(), 0);

  qInstallMessageHandler(previous);
  EXPECT_EQ(g_warning_count.load(), 0);
}

// Windows display ICC plan, W6: the converter keeps one prepared conversion for each encoding of
// its destination, and a new destination converts with its own profile.
TEST_F(WindowDisplayDestinationTest, ConverterFollowsTheDestinationOfEachEncoding) {
  QImage source(40, 30, QImage::Format_RGBA8888);
  for (int y = 0; y < source.height(); ++y) {
    uchar* row = source.scanLine(y);
    for (int x = 0; x < source.width(); ++x) {
      row[x * 4 + 0] = static_cast<uchar>(x * 6);
      row[x * 4 + 1] = static_cast<uchar>(y * 8);
      row[x * 4 + 2] = static_cast<uchar>((x * y) % 256);
      row[x * 4 + 3] = 255;
    }
  }
  const auto expected = [&source](const ViewerDisplayConfig&    encoding,
                                  const std::vector<std::byte>& profile) {
    QImage image = source.copy();
    ConvertImageToDisplay(image, encoding, profile);
    return image;
  };
  const auto destination_of = [](std::vector<std::byte> bytes) {
    auto destination           = std::make_shared<DisplayDestination>();
    destination->kind          = DisplayDestinationKind::MonitorProfile;
    destination->profile_bytes = std::move(bytes);
    return std::shared_ptr<const DisplayDestination>(std::move(destination));
  };
  const ViewerDisplayConfig rec709{ColorUtils::ColorSpace::REC709, ColorUtils::EOTF::GAMMA_2_2};
  const ViewerDisplayConfig p3{ColorUtils::ColorSpace::P3_D65, ColorUtils::EOTF::SRGB_PIECEWISE};
  const auto                p3_gamma22 = P3ProfileBytes(2.2);
  const auto                p3_gamma18 = P3ProfileBytes(1.8);

  DisplayImageConverter     converter;
  EXPECT_EQ(converter.Convert(source, rec709), source) << "no destination converts nothing";
  converter.SetDestination(destination_of(p3_gamma22));
  // Each encoding twice: the second request uses the kept conversion.
  for (int i = 0; i < 2; ++i) {
    EXPECT_EQ(converter.Convert(source, rec709), expected(rec709, p3_gamma22));
    EXPECT_EQ(converter.Convert(source, p3), expected(p3, p3_gamma22));
  }
  converter.SetDestination(destination_of(p3_gamma18));
  EXPECT_NE(expected(rec709, p3_gamma18), expected(rec709, p3_gamma22));
  EXPECT_EQ(converter.Convert(source, rec709), expected(rec709, p3_gamma18))
      << "a new destination does not use the conversion of the old one";
  converter.SetDestination(nullptr);
  EXPECT_EQ(converter.Convert(source, rec709), source);
}

}  // namespace
}  // namespace alcedo
