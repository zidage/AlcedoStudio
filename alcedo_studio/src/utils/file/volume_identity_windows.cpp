//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <array>
#include <cwchar>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winnetwk.h>

#include "utils/file/volume_identity.hpp"

namespace alcedo {
namespace {

auto ToUtf8(std::wstring_view text) -> std::string {
  if (text.empty()) return {};
  const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                         nullptr, 0, nullptr, nullptr);
  if (size <= 0) return {};
  std::string utf8(static_cast<std::size_t>(size), '\0');
  ::WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), size,
                        nullptr, nullptr);
  return utf8;
}

/// `\\server\share` in lower case from a UNC path `\\server\share[\...]` or
/// `\\?\UNC\server\share[\...]`. Empty when the path names no share.
auto ShareIdentity(std::wstring_view path) -> std::string {
  constexpr std::wstring_view kLongUncPrefix = L"\\\\?\\UNC\\";
  if (path.starts_with(kLongUncPrefix)) {
    path.remove_prefix(kLongUncPrefix.size());
  } else if (path.starts_with(L"\\\\") && !path.starts_with(L"\\\\?\\")) {
    path.remove_prefix(2);
  } else {
    return {};
  }
  const auto server_end = path.find(L'\\');
  if (server_end == std::wstring_view::npos || server_end == 0) return {};
  const auto share_end = path.find(L'\\', server_end + 1);
  const auto share     = path.substr(0, share_end);
  if (share.size() <= server_end + 1) return {};

  // Server and share names are case-insensitive.
  std::wstring identity = L"\\\\" + std::wstring(share);
  ::CharLowerBuffW(identity.data(), static_cast<DWORD>(identity.size()));
  return ToUtf8(identity);
}

/// Share identity of a mapped network drive such as `Z:\`.
auto MappedDriveShareIdentity(std::wstring_view volume_path) -> std::string {
  if (volume_path.size() < 2 || volume_path[1] != L':') return {};
  const std::wstring drive(volume_path.substr(0, 2));
  DWORD              length = 0;
  if (::WNetGetConnectionW(drive.c_str(), nullptr, &length) != ERROR_MORE_DATA || length == 0) {
    return {};
  }
  std::vector<wchar_t> remote(length);
  if (::WNetGetConnectionW(drive.c_str(), remote.data(), &length) != NO_ERROR) return {};
  return ShareIdentity(remote.data());
}

}  // namespace

auto ReadVolumeIdentity(const std::filesystem::path& directory) -> std::string {
  std::error_code ec;
  if (directory.empty() || !std::filesystem::is_directory(directory, ec) || ec) return {};
  const auto absolute = std::filesystem::absolute(directory, ec);
  if (ec) return {};
  const std::wstring&  native = absolute.native();

  // The volume path (`C:\`, `\\server\share\`, or a mount folder) is never longer than the input.
  std::vector<wchar_t> volume_path(native.size() + 2);
  if (!::GetVolumePathNameW(native.c_str(), volume_path.data(),
                            static_cast<DWORD>(volume_path.size()))) {
    return {};
  }
  const std::wstring_view volume(volume_path.data());

  if (::GetDriveTypeW(volume_path.data()) == DRIVE_REMOTE) {
    const auto share = ShareIdentity(volume);
    return share.empty() ? MappedDriveShareIdentity(volume) : share;
  }

  // A volume GUID path is `\\?\Volume{36-character GUID}\`: 49 characters and the terminator.
  std::array<wchar_t, 64> volume_name{};
  if (!::GetVolumeNameForVolumeMountPointW(volume_path.data(), volume_name.data(),
                                           static_cast<DWORD>(volume_name.size()))) {
    return {};
  }
  return ToUtf8(volume_name.data());
}

auto ReadVolumeLabel(const std::filesystem::path& directory) -> VolumeLabel {
  std::error_code ec;
  if (directory.empty() || !std::filesystem::is_directory(directory, ec) || ec) return {};
  const auto absolute = std::filesystem::absolute(directory, ec);
  if (ec) return {};
  const std::wstring&  native = absolute.native();

  // The volume path (`C:\`, `\\server\share\`, or a mount folder) is never longer than the input.
  std::vector<wchar_t> volume_path(native.size() + 2);
  if (!::GetVolumePathNameW(native.c_str(), volume_path.data(),
                            static_cast<DWORD>(volume_path.size()))) {
    return {};
  }
  VolumeLabel label;
  label.mount_point = std::filesystem::path(volume_path.data());
  // A volume label has at most 32 characters; MAX_PATH + 1 is the documented buffer size.
  std::array<wchar_t, MAX_PATH + 1> name{};
  if (::GetVolumeInformationW(volume_path.data(), name.data(), static_cast<DWORD>(name.size()),
                              nullptr, nullptr, nullptr, nullptr, 0)) {
    label.name = ToUtf8(name.data());
  }
  return label;
}

auto ListVolumeMountPoints() -> std::vector<VolumeMountPoint> {
  std::vector<VolumeMountPoint> mount_points;

  // Local volumes: the GUID path is the identity; each drive letter or mount folder is a mount
  // point. A volume GUID path is 49 characters and the terminator.
  std::array<wchar_t, 64>       volume_name{};
  HANDLE search = ::FindFirstVolumeW(volume_name.data(), static_cast<DWORD>(volume_name.size()));
  if (search != INVALID_HANDLE_VALUE) {
    do {
      DWORD length = 0;
      ::GetVolumePathNamesForVolumeNameW(volume_name.data(), nullptr, 0, &length);
      if (::GetLastError() != ERROR_MORE_DATA || length == 0) continue;
      std::vector<wchar_t> paths(length);
      if (!::GetVolumePathNamesForVolumeNameW(volume_name.data(), paths.data(), length, &length)) {
        continue;
      }
      const auto identity = ToUtf8(volume_name.data());
      // A list of strings, each with its terminator, and one more terminator at the end.
      for (const wchar_t* path = paths.data(); *path != L'\0'; path += std::wcslen(path) + 1) {
        mount_points.push_back(VolumeMountPoint{identity, std::filesystem::path(path)});
      }
    } while (::FindNextVolumeW(search, volume_name.data(), static_cast<DWORD>(volume_name.size())));
    ::FindVolumeClose(search);
  }

  // Mapped network drives: the share is the identity; the drive is the mount point.
  const DWORD drives = ::GetLogicalDrives();
  for (wchar_t letter = L'A'; letter <= L'Z'; ++letter) {
    if ((drives & (1u << (letter - L'A'))) == 0) continue;
    const std::wstring drive{letter, L':', L'\\'};
    if (::GetDriveTypeW(drive.c_str()) != DRIVE_REMOTE) continue;
    auto identity = MappedDriveShareIdentity(drive);
    if (!identity.empty()) {
      mount_points.push_back(VolumeMountPoint{std::move(identity), std::filesystem::path(drive)});
    }
  }
  return mount_points;
}

auto ListMountPointPaths() -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> mount_points;
  // A list of `X:\` strings, each with its terminator, and one more terminator at the end.
  std::array<wchar_t, 4 * 26 + 1>    drives{};
  const DWORD length = ::GetLogicalDriveStringsW(static_cast<DWORD>(drives.size()), drives.data());
  if (length == 0 || length >= drives.size()) return mount_points;
  for (const wchar_t* drive = drives.data(); *drive != L'\0'; drive += std::wcslen(drive) + 1) {
    mount_points.emplace_back(drive);
  }
  return mount_points;
}

}  // namespace alcedo
