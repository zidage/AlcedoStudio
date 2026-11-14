//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

// The ICC profile that Windows associates with the monitor of a window. Windows only.

#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace alcedo {

struct MonitorColorProfile {
  /// GDI device name of the monitor, for example `\\.\DISPLAY1`.
  std::wstring          device_name;
  /// File of the default ICC profile of the monitor for the current user. Empty when Windows has
  /// no profile for the monitor; Windows then treats the monitor as sRGB.
  std::filesystem::path profile_path;
  /// True when Windows Advanced Color is active on the monitor (HDR or Auto Color Management).
  /// The Desktop Window Manager then interprets SDR application pixels as sRGB and converts them
  /// to the panel, so the destination of the application is sRGB and not `profile_path`.
  bool                  advanced_color_active = false;
};

/**
 * @brief Read the color profile state of the monitor that shows most of a window.
 *
 * The profile is the one that `GetICMProfileW` reports for the device context of the monitor: the
 * per-user default of the Windows color system. The file is not read here.
 *
 * @param native_window The `HWND` of a top-level window (`QWindow::winId()`).
 * @return std::nullopt when @p native_window is not a window, such as the dummy id of the Qt
 *         offscreen platform, or when the monitor of the window cannot be read.
 * @thread Any thread.
 */
[[nodiscard]] auto QueryMonitorColorProfile(void* native_window)
    -> std::optional<MonitorColorProfile>;

}  // namespace alcedo
