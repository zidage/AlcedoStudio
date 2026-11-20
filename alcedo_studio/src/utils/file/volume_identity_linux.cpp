//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <sys/stat.h>

#include <filesystem>
#include <string>
#include <system_error>

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

}  // namespace alcedo
