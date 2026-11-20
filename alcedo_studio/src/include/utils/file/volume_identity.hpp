//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace alcedo {

/**
 * @brief Read a stable identity of the volume that holds @p directory.
 *
 * The identity stays the same when the volume gets a new drive letter or mount point, so a mount
 * can find its volume again (Mounted Folder Plan, MF8). The source of the value:
 *
 * - Windows, local volume: the volume GUID path of `GetVolumeNameForVolumeMountPointW`, for
 *   example `\\?\Volume{0b6c...}\`.
 * - Windows, network share (a UNC path or a mapped drive): `\\server\share` in lower case.
 * - macOS: `NSURLVolumeUUIDStringKey`.
 * - Linux: the name of the `/dev/disk/by-uuid` entry whose block device holds the directory.
 *
 * Thread safety: the function holds no state; any thread can call it.
 *
 * @return The identity as UTF-8 text, or an empty string when the volume has no readable
 *         identity or the directory does not exist. The function does not throw.
 */
[[nodiscard]] auto ReadVolumeIdentity(const std::filesystem::path& directory) -> std::string;

/// One mounted volume: its identity in the form of ReadVolumeIdentity and one path where it is
/// mounted.
struct VolumeMountPoint {
  std::string           identity;
  std::filesystem::path mount_point;

  auto                  operator==(const VolumeMountPoint&) const -> bool = default;
};

/**
 * @brief List the mounted volumes that have an identity, with each of their mount points.
 *
 * A mount whose root directory is unreachable searches this list for its recorded volume
 * identity (Mounted Folder Plan, MF8). The source of the list:
 *
 * - Windows: each local volume of `FindFirstVolumeW` with each path of
 *   `GetVolumePathNamesForVolumeNameW` (drive letters and mount folders), and each mapped
 *   network drive with its share identity.
 * - macOS: `mountedVolumeURLsIncludingResourceValuesForKeys` with `NSURLVolumeUUIDStringKey`.
 * - Linux: each `/proc/self/mounts` entry whose device is a `/dev/disk/by-uuid` entry.
 *
 * A volume with two mount points has two entries. A volume without an identity has none.
 *
 * Thread safety: the function holds no state; any thread can call it. It reads the volume list
 * of the system, so call it from a worker thread.
 *
 * @return The entries in the order of the system list. The function does not throw.
 */
[[nodiscard]] auto ListVolumeMountPoints() -> std::vector<VolumeMountPoint>;

}  // namespace alcedo
