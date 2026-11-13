//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file display_destination_test_support.hpp
/// @brief Display destinations for the image store tests of the Windows display ICC plan (W5):
///        a P3 monitor profile, destinations without a window, and the expected conversion.

#pragma once

#include <gtest/gtest.h>
#include <lcms2.h>

#include <QImage>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include "color/color_encoding_catalog.hpp"
#include "ui/edit_viewer/display_color_transform.hpp"
#include "ui/edit_viewer/window_display_destination.hpp"

namespace alcedo::test {

/// The bytes of a P3-D65 gamma 2.2 display profile: Rec.709 pixels change on it.
inline auto P3DisplayProfileBytes() -> std::vector<std::byte> {
  const auto&           xy = color::GamutPrimariesXy(color::ColorGamutId::P3D65);
  const cmsCIExyY       white{xy[6], xy[7], 1.0};
  const cmsCIExyYTRIPLE primaries{{xy[0], xy[1], 1.0}, {xy[2], xy[3], 1.0}, {xy[4], xy[5], 1.0}};
  cmsToneCurve*         curve     = cmsBuildGamma(nullptr, 2.2);
  cmsToneCurve*         curves[3] = {curve, curve, curve};
  cmsHPROFILE           profile   = cmsCreateRGBProfile(&white, &primaries, curves);
  cmsFreeToneCurve(curve);
  cmsUInt32Number size = 0;
  EXPECT_TRUE(cmsSaveProfileToMem(profile, nullptr, &size));
  std::vector<std::byte> bytes(size);
  EXPECT_TRUE(cmsSaveProfileToMem(profile, bytes.data(), &size));
  cmsCloseProfile(profile);
  return bytes;
}

/// A published destination of @p kind with @p bytes.
inline auto MakeDestination(DisplayDestinationKind kind, std::vector<std::byte> bytes = {})
    -> std::shared_ptr<const DisplayDestination> {
  auto destination           = std::make_shared<DisplayDestination>();
  destination->kind          = kind;
  destination->profile_bytes = std::move(bytes);
  destination->description   = "test destination";
  return destination;
}

/// A monitor whose profile is the file @p profile_path.
inline auto MonitorWithProfile(std::filesystem::path profile_path) -> MonitorColorProfile {
  MonitorColorProfile monitor;
  monitor.device_name  = L"\\\\.\\DISPLAY7";
  monitor.profile_path = std::move(profile_path);
  return monitor;
}

/// Write @p bytes to @p path.
inline void WriteProfileFile(const std::filesystem::path&  path,
                             const std::vector<std::byte>& bytes) {
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  file.write(reinterpret_cast<const char*>(bytes.data()),
             static_cast<std::streamsize>(bytes.size()));
  ASSERT_TRUE(file.good());
}

/// The expected display pixels: ConvertImageToDisplay of a copy of @p source.
inline auto ExpectedDisplayImage(const QImage& source, const ViewerDisplayConfig& encoding,
                                 const std::vector<std::byte>& bytes) -> QImage {
  QImage expected = source.copy();
  ConvertImageToDisplay(expected, encoding, bytes);
  return expected;
}

/// An RGBA8 image whose pixels cover the code range, so that a conversion changes most of them.
inline auto GradientImage(int width, int height) -> QImage {
  QImage image(width, height, QImage::Format_RGBA8888);
  for (int y = 0; y < height; ++y) {
    auto* row = image.scanLine(y);
    for (int x = 0; x < width; ++x) {
      row[x * 4 + 0] = static_cast<uchar>((x * 255) / (width > 1 ? width - 1 : 1));
      row[x * 4 + 1] = static_cast<uchar>((y * 255) / (height > 1 ? height - 1 : 1));
      row[x * 4 + 2] = static_cast<uchar>(((x + y) * 97) % 256);
      row[x * 4 + 3] = 255;
    }
  }
  return image;
}

}  // namespace alcedo::test
