//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

/// @file monitor_color_profile_test.cpp
/// @brief W2 of the Windows display ICC plan: the Windows color system query of a monitor
///        profile, on the monitors of the test machine. Windows only.

#include <gtest/gtest.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <filesystem>
#include <iostream>
#include <string>

#include "ui/edit_viewer/monitor_color_profile.hpp"
#include "ui/edit_viewer/window_display_destination.hpp"

namespace alcedo {
namespace {

auto PathText(const std::filesystem::path& path) -> std::string {
  const auto text = path.u8string();
  return std::string(text.begin(), text.end());
}

auto KindText(DisplayDestinationKind kind) -> const char* {
  switch (kind) {
    case DisplayDestinationKind::MonitorProfile:
      return "MonitorProfile";
    case DisplayDestinationKind::SrgbAdvancedColor:
      return "SrgbAdvancedColor";
    case DisplayDestinationKind::SrgbNoProfile:
      return "SrgbNoProfile";
    case DisplayDestinationKind::Unusable:
      return "Unusable";
    case DisplayDestinationKind::NoWindow:
      return "NoWindow";
  }
  return "?";
}

TEST(MonitorColorProfileTest, PrimaryMonitorReportsADeviceName) {
  const auto monitor = QueryMonitorColorProfile(GetDesktopWindow());
  ASSERT_TRUE(monitor.has_value());
  EXPECT_FALSE(monitor->device_name.empty());
  if (!monitor->profile_path.empty()) {
    EXPECT_TRUE(std::filesystem::is_regular_file(monitor->profile_path))
        << PathText(monitor->profile_path);
  }

  // The issue requirement: the query gives a readable RGB display profile (or the sRGB
  // destination of D9 and D10) for the primary monitor.
  const auto destination = ResolveDisplayDestination(*monitor);
  std::wcout << L"primary monitor " << monitor->device_name << L"\n";
  std::cout << "  profile: " << PathText(monitor->profile_path) << "\n"
            << "  advanced color: " << (monitor->advanced_color_active ? "active" : "inactive")
            << "\n"
            << "  destination: " << KindText(destination.kind) << " '" << destination.description
            << "' " << destination.error << "\n";
  EXPECT_NE(destination.kind, DisplayDestinationKind::Unusable) << destination.error;
  EXPECT_NE(destination.kind, DisplayDestinationKind::NoWindow);
  EXPECT_FALSE(destination.profile_bytes.empty());
}

TEST(MonitorColorProfileTest, HandleThatIsNoWindowHasNoProfile) {
  EXPECT_FALSE(QueryMonitorColorProfile(nullptr).has_value());
  EXPECT_FALSE(QueryMonitorColorProfile(reinterpret_cast<void*>(1)).has_value());
}

}  // namespace
}  // namespace alcedo
