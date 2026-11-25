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

/// A new mount point of the mount table (#1076). The Disk section of the Collections panel shows
/// a card for a volume that holds no mount.
struct ArrivedVolume {
  /// The new mount point: a drive such as `E:\` on Windows, the mount path on macOS and Linux.
  std::filesystem::path mount_point;
  /// The display name of the volume (ReadVolumeLabel); empty when it has none.
  std::string           name;
  /// The identity of the volume (ReadVolumeIdentity); empty when it has none.
  std::string           identity;
  /// True when the root of the volume holds a `DCIM` directory, as a camera card does.
  bool                  has_dcim_directory = false;

  auto                  operator==(const ArrivedVolume&) const -> bool = default;
};

/// The values of the volume at a new mount point. ReadArrivedVolume in production; a test
/// injects them.
using ArrivedVolumeReader = std::function<ArrivedVolume(const std::filesystem::path&)>;

/// ReadVolumeLabel, ReadVolumeIdentity, and the `DCIM` check for the mount point @p mount_point.
/// It reads the volume, which has just arrived and answers; the watchers call it on the UI
/// thread once for each new mount point.
[[nodiscard]] auto ReadArrivedVolume(const std::filesystem::path& mount_point) -> ArrivedVolume;

/**
 * @brief Reports change notifications of the directories of the mounts (Mounted Folder Plan,
 *        MF7).
 *
 * A notification only names the mount. The scan reads the directory listing for the result
 * (P8), so a lost or a spurious notification changes no data.
 *
 * The watcher also reports the volume events (#1020): the removal of the volume of a watched
 * mount, and the arrival of a volume, so an offline mount gets a scan when its drive is
 * connected again. It reports each new mount point of the mount table with the values of its
 * volume (#1076), so the owner of the mounts can show a volume that holds no mount, and each
 * mount point that is gone (#1087), so the owner closes the card of that volume.
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
  /// A mount point is new in the mount table. The watcher does not know whether the volume
  /// holds a mount; the owner of the mounts compares it with them.
  using MountPointArrivedCallback = std::function<void(const ArrivedVolume&)>;
  /// A mount point is gone from the mount table (#1087): its volume was ejected or pulled out.
  using MountPointRemovedCallback = std::function<void(const std::filesystem::path&)>;

  virtual ~MountDirectoryWatcher()                        = default;

  /// Set the callback of the change notifications. Set it before the first Watch.
  virtual void SetChangeCallback(ChangeCallback callback) = 0;

  /// Set the callbacks of the volume events. Set them before the first Watch. The mount table
  /// of this call is the state that the first comparison uses, so a mount point that exists
  /// now is not reported.
  virtual void SetVolumeCallbacks(VolumeRemovedCallback removed, VolumeArrivedCallback arrived,
                                  MountPointArrivedCallback mount_point_arrived,
                                  MountPointRemovedCallback mount_point_removed) = 0;

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
 * device interface notification of the volume class (GUID_DEVINTERFACE_VOLUME). The drive
 * letter of the volume can come after that notification, so the new mount points come from the
 * drive list (ListMountPointPaths), which the watcher reads at each notification and every
 * kMountTablePollInterval.
 *
 * Other platforms: QFileSystemWatcher on the root and on each listed directory, at most
 * kMountWatcherDirectoryLimit directories for all mounts. A mount that passes the limit is not
 * watched. The watcher reads the mount table (ListMountPointPaths) every
 * kMountTablePollInterval: a new mount point is an arrival, and a mount point that is gone
 * removes each watched mount whose root is at or below it.
 *
 * Each new mount point goes to the MountPointArrivedCallback with the values of
 * ReadArrivedVolume, and each mount point that is gone to the MountPointRemovedCallback. On
 * Windows the removal of a volume device also reads the drive list.
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
 * @param read_arrived_volume The values of the volume at a new mount point; empty takes
 *        ReadArrivedVolume.
 */
auto CreateQtMountDirectoryWatcher(
    std::size_t               directory_limit     = kMountWatcherDirectoryLimit,
    MountTableReader          read_mount_table    = {},
    std::chrono::milliseconds poll_interval       = kMountTablePollInterval,
    ArrivedVolumeReader       read_arrived_volume = {}) -> std::unique_ptr<MountDirectoryWatcher>;

/**
 * @brief The comparison of two reads of the mount table, for the volume events of both watchers.
 *
 * Start reads the table that the first comparison uses. Each Poll reads the table again and
 * returns the mount points that are new and the ones that are gone since the last read.
 *
 * Thread safety: one thread.
 */
class MountTableComparison {
 public:
  struct Changes {
    std::vector<std::filesystem::path> arrived;
    std::vector<std::filesystem::path> removed;
  };

  /// @param read_mount_table The mount table; empty takes ListMountPointPaths.
  explicit MountTableComparison(MountTableReader read_mount_table = {});

  /// Read the table that the first Poll compares with.
  void               Start();
  /// Read the table and compare it with the last read. Empty before Start.
  [[nodiscard]] auto Poll() -> Changes;

 private:
  MountTableReader                   read_mount_table_;
  std::vector<std::filesystem::path> mount_points_;
  bool                               started_ = false;
};

}  // namespace alcedo::ui
