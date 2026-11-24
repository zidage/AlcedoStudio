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

#include "utils/file/mount_directory_listing.hpp"
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

/// A `/dev/disk/by-label` entry name: udev writes a character outside `[A-Za-z0-9#+-.:=@_]` as a
/// `\xHH` escape, for example `\x20` for a space.
auto UnescapeLabelName(const std::string& name) -> std::string {
  std::string text;
  for (std::size_t i = 0; i < name.size(); ++i) {
    if (name[i] == '\\' && i + 3 < name.size() && name[i + 1] == 'x' &&
        std::isxdigit(static_cast<unsigned char>(name[i + 2])) &&
        std::isxdigit(static_cast<unsigned char>(name[i + 3]))) {
      text.push_back(static_cast<char>(std::stoi(name.substr(i + 2, 2), nullptr, 16)));
      i += 3;
      continue;
    }
    text.push_back(name[i]);
  }
  return text;
}

}  // namespace

auto ReadVolumeLabel(const std::filesystem::path& directory) -> VolumeLabel {
  std::error_code ec;
  if (directory.empty() || !std::filesystem::is_directory(directory, ec) || ec) return {};
  const auto absolute = std::filesystem::weakly_canonical(directory, ec);
  if (ec) return {};
  struct stat directory_info{};
  if (::stat(absolute.c_str(), &directory_info) != 0) return {};

  VolumeLabel   label;
  // The mount point is the longest mount table entry above the directory on its device.
  std::ifstream mounts("/proc/self/mounts");
  std::string   line;
  while (std::getline(mounts, line)) {
    std::istringstream fields(line);
    std::string        device;
    std::string        mount_point;
    if (!(fields >> device >> mount_point)) continue;
    const std::filesystem::path path(UnescapeMountField(mount_point));
    struct stat                 info{};
    if (::stat(path.c_str(), &info) != 0 || info.st_dev != directory_info.st_dev) continue;
    if (!IsPathAtOrBelow(absolute, path)) continue;
    if (path.native().size() >= label.mount_point.native().size()) {
      label.mount_point = path;
    }
  }

  std::filesystem::directory_iterator it("/dev/disk/by-label", ec);
  if (!ec) {
    for (const std::filesystem::directory_iterator end; it != end; it.increment(ec)) {
      if (ec) break;
      struct stat device_info{};
      if (::stat(it->path().c_str(), &device_info) == 0 && S_ISBLK(device_info.st_mode) &&
          device_info.st_rdev == directory_info.st_dev) {
        label.name = UnescapeLabelName(it->path().filename().string());
        break;
      }
    }
  }
  if (label.name.empty() && !label.mount_point.empty()) {
    label.name = label.mount_point.filename().string();
    if (label.name.empty()) {
      label.name = label.mount_point.string();
    }
  }
  return label;
}

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

auto ListMountPointPaths() -> std::vector<std::filesystem::path> {
  std::vector<std::filesystem::path> mount_points;
  std::ifstream                      mounts("/proc/self/mounts");
  std::string                        line;
  while (std::getline(mounts, line)) {
    std::istringstream fields(line);
    std::string        device;
    std::string        mount_point;
    if (fields >> device >> mount_point) {
      mount_points.emplace_back(UnescapeMountField(mount_point));
    }
  }
  return mount_points;
}

}  // namespace alcedo
