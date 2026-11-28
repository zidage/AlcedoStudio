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

/// The display name and the mount point of the volume that holds a directory. The Disk section
/// of the Collections panel shows them for each mount (#1076).
struct VolumeLabel {
  /// The name of the volume as the system shows it. Empty when the volume has no name.
  std::string           name;
  /// The path where the volume is mounted: the drive (`E:\`) or the mount folder on Windows,
  /// the mount path on macOS and Linux. Empty when it cannot be read.
  std::filesystem::path mount_point;

  auto                  operator==(const VolumeLabel&) const -> bool = default;
};

/**
 * @brief Read the display name and the mount point of the volume that holds @p directory.
 *
 * The source of the name:
 *
 * - Windows: the volume label of `GetVolumeInformationW`. A volume without a label has an empty
 *   name (Windows then shows a name of its own, such as "Local Disk").
 * - macOS: `NSURLVolumeNameKey`, the volume name that `getattrlist` reads.
 * - Linux: the file system label (the `/dev/disk/by-label` entry of the block device of the
 *   directory), or the last component of the mount point when the file system has no label.
 *
 * The mount point comes from `GetVolumePathNameW` on Windows, `statfs` on macOS, and the
 * longest `/proc/self/mounts` entry above the directory on the same device on Linux.
 *
 * Thread safety: the function holds no state; any thread can call it. It reads the file system
 * of the directory, so call it from a worker thread.
 *
 * @return Empty values when the directory does not exist. The function does not throw.
 */
[[nodiscard]] auto ReadVolumeLabel(const std::filesystem::path& directory) -> VolumeLabel;

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

/**
 * @brief List the mount point of each mounted file system, from the mount table of the system
 *        only.
 *
 * The directory watcher of the mounts compares two lists to find a volume that arrived or was
 * removed (#1020). The function reads no file system of the list, so a network file system that
 * does not answer does not block it, and the UI thread can call it. The source of the list:
 *
 * - Windows: each drive of `GetLogicalDriveStringsW`.
 * - macOS: `getfsstat` with `MNT_NOWAIT`.
 * - Linux: the mount point field of each `/proc/self/mounts` entry.
 *
 * Thread safety: the function holds no state; any thread can call it.
 *
 * @return The mount points in the order of the system list. The function does not throw.
 */
[[nodiscard]] auto ListMountPointPaths() -> std::vector<std::filesystem::path>;

}  // namespace alcedo
