//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>

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
 * Thread safety: create, call, and destroy on one thread with an event loop (the UI thread).
 * The change callback runs on that thread.
 */
class MountDirectoryWatcher {
 public:
  using ChangeCallback                                    = std::function<void(mount_id_t)>;

  virtual ~MountDirectoryWatcher()                        = default;

  /// Set the callback of the change notifications. Set it before the first Watch.
  virtual void SetChangeCallback(ChangeCallback callback) = 0;

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
 * has subdirectories. The watcher closes the handles of a volume that the user ejects.
 *
 * Other platforms: QFileSystemWatcher on the root and on each listed directory, at most
 * kMountWatcherDirectoryLimit directories for all mounts. A mount that passes the limit is not
 * watched.
 */
auto CreatePlatformMountDirectoryWatcher() -> std::unique_ptr<MountDirectoryWatcher>;

/// The QFileSystemWatcher form of the watcher, with @p directory_limit directories for all
/// mounts. The platform watcher of macOS and Linux.
auto CreateQtMountDirectoryWatcher(std::size_t directory_limit = kMountWatcherDirectoryLimit)
    -> std::unique_ptr<MountDirectoryWatcher>;

}  // namespace alcedo::ui
