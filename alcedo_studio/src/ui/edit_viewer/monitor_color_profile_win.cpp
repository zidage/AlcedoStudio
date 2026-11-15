//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/edit_viewer/monitor_color_profile.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cwchar>
#include <vector>

namespace alcedo {
namespace {

/// Default ICC profile of the monitor @p device_name, or an empty path when it has none.
auto ReadDefaultProfilePath(const wchar_t* device_name) -> std::filesystem::path {
  const HDC device_context = CreateDCW(device_name, nullptr, nullptr, nullptr);
  if (device_context == nullptr) {
    return {};
  }
  std::filesystem::path path;
  DWORD                 length = 0;
  // The first call reports the length of the file name with its terminator.
  GetICMProfileW(device_context, &length, nullptr);
  if (length > 0) {
    std::wstring name(length, L'\0');
    if (GetICMProfileW(device_context, &length, name.data())) {
      name.resize(std::wcslen(name.c_str()));
      path = name;
    }
  }
  DeleteDC(device_context);
  return path;
}

/// True when Advanced Color is active on the display path whose source is @p device_name.
auto IsAdvancedColorActive(const wchar_t* device_name) -> bool {
  std::vector<DISPLAYCONFIG_PATH_INFO> paths;
  std::vector<DISPLAYCONFIG_MODE_INFO> modes;
  LONG                                 result = ERROR_INSUFFICIENT_BUFFER;
  // The path count can change between the two calls when a monitor is connected.
  for (int attempt = 0; attempt < 4 && result == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
    UINT32 path_count = 0;
    UINT32 mode_count = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) !=
        ERROR_SUCCESS) {
      return false;
    }
    paths.resize(path_count);
    modes.resize(mode_count);
    result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count,
                                modes.data(), nullptr);
    paths.resize(path_count);
  }
  if (result != ERROR_SUCCESS) {
    return false;
  }
  for (const auto& path : paths) {
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
    source.header.size      = sizeof(source);
    source.header.adapterId = path.sourceInfo.adapterId;
    source.header.id        = path.sourceInfo.id;
    if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
        std::wcscmp(source.viewGdiDeviceName, device_name) != 0) {
      continue;
    }
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO color{};
    color.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
    color.header.size      = sizeof(color);
    color.header.adapterId = path.targetInfo.adapterId;
    color.header.id        = path.targetInfo.id;
    if (DisplayConfigGetDeviceInfo(&color.header) == ERROR_SUCCESS && color.advancedColorEnabled) {
      return true;
    }
  }
  return false;
}

}  // namespace

auto QueryMonitorColorProfile(void* native_window) -> std::optional<MonitorColorProfile> {
  const HWND window = static_cast<HWND>(native_window);
  if (window == nullptr || !IsWindow(window)) {
    return std::nullopt;
  }
  const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
  MONITORINFOEXW info{};
  info.cbSize = sizeof(info);
  if (monitor == nullptr || !GetMonitorInfoW(monitor, &info)) {
    return std::nullopt;
  }
  MonitorColorProfile profile;
  profile.device_name           = info.szDevice;
  profile.profile_path          = ReadDefaultProfilePath(info.szDevice);
  profile.advanced_color_active = IsAdvancedColorActive(info.szDevice);
  return profile;
}

}  // namespace alcedo
