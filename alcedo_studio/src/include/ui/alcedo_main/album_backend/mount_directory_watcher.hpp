//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "type/type.hpp"

namespace alcedo::ui {

/// Directories of all mounts that the per-directory watcher holds (Mounted Folder Plan, section
/// 14, watcher directory limit).
inline constexpr std::size_t kMountWatcherDirectoryLimit = 4096;

/**
 * @brief Reports change notifications of the directories of the mounts (Mounted Folder Plan,
 *        MF7).
 *
 * A notification only names the mount. The scan reads the directory listing for the result
 * (P8), so a lost or a spurious notification changes no data.
 *
 * The watcher also reports the volume events (#1020): the removal of the volume of a watched
 * mount, and the arrival of a volume, so an offline mount gets a scan when its drive is
 * connected again.
 *
 * Thread safety: create, call, and destroy on one thread with an event loop (the UI thread).
 * The callbacks run on that thread.
 */
class MountDirectoryWatcher {
 public:
  using ChangeCallback                                    = std::function<void(mount_id_t)>;
  /// The volume of the watched mount was removed, or its removal was canceled. The watch of the
  /// mount has ended when the callback runs; the scan finds the mount offline or watches it
  /// again.
  using VolumeRemovedCallback                             = std::function<void(mount_id_t)>;
  /// A volume was mounted. The watcher does not know which mount it holds.
  using VolumeArrivedCallback                             = std::function<void()>;

  virtual ~MountDirectoryWatcher()                        = default;

  /// Set the callback of the change notifications. Set it before the first Watch.
  virtual void SetChangeCallback(ChangeCallback callback) = 0;

  /// Set the callbacks of the volume events. Set them before the first Watch.
  virtual void SetVolumeCallbacks(VolumeRemovedCallback removed,
                                  VolumeArrivedCallback arrived) = 0;

  /**
   * @brief Watch the root directory @p root of the mount @p mount_id and the directories
   *        below it.
   *
   * The call replaces the earlier watch of the mount.
   *
   * @param directories Each directory below @p root as a relative path (the listing of the last
   *                    scan). A watcher that watches the whole tree from the root can ignore it.
   * @return False when the mount is not watched: a registration failed, or the directories pass
   *         the limit of the watcher. The mount then has no watch.
   */
  virtual auto Watch(mount_id_t mount_id, const std::filesystem::path& root,
                     std::span<const std::wstring> directories) -> bool = 0;

  /// Remove the watch of the mount. No effect for a mount with no watch.
  virtual void Unwatch(mount_id_t mount_id)                             = 0;

  /// Remove the watch of every mount.
  virtual void UnwatchAll()                                             = 0;
};

/**
 * @brief The watcher of this platform.
 *
 * Windows: one change notification handle for each root directory that also reports the
 * directories below it. A handle in a directory prevents a rename of each directory above it,
 * so a handle for each directory would prevent the user from renaming a mounted directory that
 * has subdirectories. The watcher closes the handles of a volume that the user ejects, and
 * reports the removal when the eject completes or is canceled. A volume arrives with the
 * device interface notification of the volume class (GUID_DEVINTERFACE_VOLUME).
 *
 * Other platforms: QFileSystemWatcher on the root and on each listed directory, at most
 * kMountWatcherDirectoryLimit directories for all mounts. A mount that passes the limit is not
 * watched. The watcher reads the mount table (ListMountPointPaths) every
 * kMountTablePollInterval: a new mount point is an arrival, and a mount point that is gone
 * removes each watched mount whose root is at or below it.
 */
auto CreatePlatformMountDirectoryWatcher() -> std::unique_ptr<MountDirectoryWatcher>;

/// The mount points of the system for the volume events of the QFileSystemWatcher form.
/// ListMountPointPaths in production; a test injects a list.
using MountTableReader = std::function<std::vector<std::filesystem::path>()>;

/// Time between two reads of the mount table by the QFileSystemWatcher form of the watcher.
inline constexpr std::chrono::milliseconds kMountTablePollInterval{2000};

/**
 * @brief The QFileSystemWatcher form of the watcher, with @p directory_limit directories for all
 *        mounts. The platform watcher of macOS and Linux.
 * @param read_mount_table The mount table; empty takes ListMountPointPaths.
 * @param poll_interval Time between two reads of the mount table. The reads start with
 *        SetVolumeCallbacks.
 */
auto CreateQtMountDirectoryWatcher(
    std::size_t               directory_limit  = kMountWatcherDirectoryLimit,
    MountTableReader          read_mount_table = {},
    std::chrono::milliseconds poll_interval    = kMountTablePollInterval)
    -> std::unique_ptr<MountDirectoryWatcher>;

}  // namespace alcedo::ui
