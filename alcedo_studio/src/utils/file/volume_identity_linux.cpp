//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <sys/stat.h>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "utils/file/volume_identity.hpp"

namespace alcedo {

auto ReadVolumeIdentity(const std::filesystem::path& directory) -> std::string {
  std::error_code ec;
  if (directory.empty() || !std::filesystem::is_directory(directory, ec) || ec) return {};
  struct stat directory_info{};
  if (::stat(directory.c_str(), &directory_info) != 0) return {};

  // Each /dev/disk/by-uuid entry is a link to the block device of one file system. A file system
  // without a block device (tmpfs, overlay, a network share, a btrfs subvolume) matches no entry.
  std::filesystem::directory_iterator it("/dev/disk/by-uuid", ec);
  if (ec) return {};
  for (const std::filesystem::directory_iterator end; it != end; it.increment(ec)) {
    if (ec) return {};
    struct stat device_info{};
    if (::stat(it->path().c_str(), &device_info) != 0 || !S_ISBLK(device_info.st_mode)) continue;
    if (device_info.st_rdev == directory_info.st_dev) {
      return it->path().filename().string();
    }
  }
  return {};
}

namespace {

/// A mount point field of /proc/self/mounts: a space, tab, newline, or backslash is an octal
/// escape such as `\040`.
auto UnescapeMountField(const std::string& field) -> std::string {
  std::string text;
  for (std::size_t i = 0; i < field.size(); ++i) {
    if (field[i] == '\\' && i + 3 < field.size() &&
        std::isdigit(static_cast<unsigned char>(field[i + 1])) &&
        std::isdigit(static_cast<unsigned char>(field[i + 2])) &&
        std::isdigit(static_cast<unsigned char>(field[i + 3]))) {
      text.push_back(static_cast<char>(std::stoi(field.substr(i + 1, 3), nullptr, 8)));
      i += 3;
      continue;
    }
    text.push_back(field[i]);
  }
  return text;
}

}  // namespace

auto ListVolumeMountPoints() -> std::vector<VolumeMountPoint> {
  std::vector<VolumeMountPoint>       mount_points;
  std::map<dev_t, std::string>        identity_of_device;
  std::error_code                     ec;
  std::filesystem::directory_iterator it("/dev/disk/by-uuid", ec);
  if (ec) return mount_points;
  for (const std::filesystem::directory_iterator end; it != end; it.increment(ec)) {
    if (ec) break;
    struct stat device_info{};
    if (::stat(it->path().c_str(), &device_info) == 0 && S_ISBLK(device_info.st_mode)) {
      identity_of_device.emplace(device_info.st_rdev, it->path().filename().string());
    }
  }

  std::ifstream mounts("/proc/self/mounts");
  std::string   line;
  while (std::getline(mounts, line)) {
    std::istringstream fields(line);
    std::string        device;
    std::string        mount_point;
    if (!(fields >> device >> mount_point)) continue;
    const auto  path = UnescapeMountField(mount_point);
    struct stat info{};
    if (::stat(path.c_str(), &info) != 0) continue;
    const auto identity = identity_of_device.find(info.st_dev);
    if (identity != identity_of_device.end()) {
      mount_points.push_back(VolumeMountPoint{identity->second, std::filesystem::path(path)});
    }
  }
  return mount_points;
}

}  // namespace alcedo
