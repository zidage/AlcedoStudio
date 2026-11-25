//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <set>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

#include "ui/alcedo_main/album_backend/mount_directory_watcher.hpp"
#include "ui/alcedo_main/i18n.hpp"
#include "utils/file/mount_directory_listing.hpp"
#include "utils/file/volume_identity.hpp"

namespace alcedo::ui {
namespace {

/// QFileSystemWatcher on the root directory and on each listed directory of each mount, and a
/// poll of the mount table for the volume events.
class QtMountDirectoryWatcher final : public MountDirectoryWatcher {
 public:
  QtMountDirectoryWatcher(std::size_t directory_limit, MountTableReader read_mount_table,
                          std::chrono::milliseconds poll_interval,
                          ArrivedVolumeReader       read_arrived_volume)
      : directory_limit_(directory_limit),
        mount_table_(std::move(read_mount_table)),
        read_arrived_volume_(read_arrived_volume ? std::move(read_arrived_volume)
                                                 : ArrivedVolumeReader(&ReadArrivedVolume)) {
    QObject::connect(&watcher_, &QFileSystemWatcher::directoryChanged, &watcher_,
                     [this](const QString& path) {
                       const auto it = mount_of_path_.find(path);
                       if (it != mount_of_path_.end() && on_change_) {
                         on_change_(it->second);
                       }
                     });
    poll_timer_.setInterval(poll_interval);
    QObject::connect(&poll_timer_, &QTimer::timeout, &poll_timer_, [this] { PollMountTable(); });
  }

  void SetChangeCallback(ChangeCallback callback) override { on_change_ = std::move(callback); }

  void SetVolumeCallbacks(VolumeRemovedCallback removed, VolumeArrivedCallback arrived,
                          MountPointArrivedCallback mount_point_arrived,
                          MountPointRemovedCallback mount_point_removed) override {
    on_volume_removed_      = std::move(removed);
    on_volume_arrived_      = std::move(arrived);
    on_mount_point_arrived_ = std::move(mount_point_arrived);
    on_mount_point_removed_ = std::move(mount_point_removed);
    if (!on_volume_removed_ && !on_volume_arrived_ && !on_mount_point_arrived_ &&
        !on_mount_point_removed_) {
      poll_timer_.stop();
      return;
    }
    // The table of this call is the state that the first poll compares with.
    mount_table_.Start();
    poll_timer_.start();
  }

  auto Watch(mount_id_t mount_id, const std::filesystem::path& root,
             std::span<const std::wstring> directories) -> bool override {
    Unwatch(mount_id);
    const std::size_t count = directories.size() + 1;
    if (watched_count_ + count > directory_limit_) {
      return false;
    }
    QStringList paths;
    paths.reserve(static_cast<qsizetype>(count));
    paths.push_back(i18n::PathToQString(root));
    for (const auto& relative : directories) {
      paths.push_back(i18n::PathToQString(root / std::filesystem::path(relative)));
    }
    const QStringList failed = watcher_.addPaths(paths);
    if (!failed.isEmpty()) {
      // A partial watch would miss the changes of the failed directories.
      QStringList added;
      for (const auto& path : paths) {
        if (!failed.contains(path)) {
          added.push_back(path);
        }
      }
      if (!added.isEmpty()) {
        (void)watcher_.removePaths(added);
      }
      return false;
    }
    for (const auto& path : paths) {
      mount_of_path_[path] = mount_id;
    }
    watched_count_ += static_cast<std::size_t>(paths.size());
    paths_of_mount_[mount_id] = std::move(paths);
    root_of_mount_[mount_id]  = root;
    return true;
  }

  void Unwatch(mount_id_t mount_id) override {
    const auto it = paths_of_mount_.find(mount_id);
    if (it == paths_of_mount_.end()) {
      return;
    }
    (void)watcher_.removePaths(it->second);
    for (const auto& path : it->second) {
      mount_of_path_.erase(path);
    }
    watched_count_ -= static_cast<std::size_t>(it->second.size());
    paths_of_mount_.erase(it);
    root_of_mount_.erase(mount_id);
  }

  void UnwatchAll() override {
    while (!paths_of_mount_.empty()) {
      Unwatch(paths_of_mount_.begin()->first);
    }
  }

 private:
  /// Compare the mount table with the last read. A mount point that is gone removes the watch of
  /// each mount whose root is at or below it; a new mount point is an arrival.
  void PollMountTable() {
    const auto              changes = mount_table_.Poll();
    std::vector<mount_id_t> removed_mounts;
    for (const auto& mount_point : changes.removed) {
      for (const auto& [mount_id, root] : root_of_mount_) {
        if (IsPathAtOrBelow(root, mount_point)) {
          removed_mounts.push_back(mount_id);
        }
      }
    }

    for (const auto mount_id : removed_mounts) {
      Unwatch(mount_id);
    }
    // A callback can change the watches; the ids are a copy.
    for (const auto mount_id : removed_mounts) {
      if (on_volume_removed_) {
        on_volume_removed_(mount_id);
      }
    }
    if (on_mount_point_removed_) {
      for (const auto& mount_point : changes.removed) {
        on_mount_point_removed_(mount_point);
      }
    }
    if (!changes.arrived.empty() && on_volume_arrived_) {
      on_volume_arrived_();
    }
    if (on_mount_point_arrived_) {
      for (const auto& mount_point : changes.arrived) {
        on_mount_point_arrived_(read_arrived_volume_(mount_point));
      }
    }
  }

  std::size_t                                           directory_limit_;
  MountTableComparison                                  mount_table_;
  ArrivedVolumeReader                                   read_arrived_volume_;
  QFileSystemWatcher                                    watcher_;
  QTimer                                                poll_timer_;
  ChangeCallback                                        on_change_;
  VolumeRemovedCallback                                 on_volume_removed_;
  VolumeArrivedCallback                                 on_volume_arrived_;
  MountPointArrivedCallback                             on_mount_point_arrived_;
  MountPointRemovedCallback                             on_mount_point_removed_;
  std::unordered_map<QString, mount_id_t>               mount_of_path_;
  std::unordered_map<mount_id_t, QStringList>           paths_of_mount_;
  std::unordered_map<mount_id_t, std::filesystem::path> root_of_mount_;
  std::size_t                                           watched_count_ = 0;
};

}  // namespace

auto ReadArrivedVolume(const std::filesystem::path& mount_point) -> ArrivedVolume {
  ArrivedVolume volume;
  volume.mount_point = mount_point;
  volume.name        = ReadVolumeLabel(mount_point).name;
  volume.identity    = ReadVolumeIdentity(mount_point);
  std::error_code ec;
  volume.has_dcim_directory = std::filesystem::is_directory(mount_point / "DCIM", ec) && !ec;
  return volume;
}

MountTableComparison::MountTableComparison(MountTableReader read_mount_table)
    : read_mount_table_(read_mount_table ? std::move(read_mount_table)
                                         : MountTableReader(&ListMountPointPaths)) {}

void MountTableComparison::Start() {
  mount_points_ = read_mount_table_();
  started_      = true;
}

auto MountTableComparison::Poll() -> Changes {
  Changes changes;
  if (!started_) {
    return changes;
  }
  auto                            current = read_mount_table_();
  const std::set<std::filesystem::path> last(mount_points_.begin(), mount_points_.end());
  const std::set<std::filesystem::path> now(current.begin(), current.end());
  // The order of the system list; a mount point that the list holds twice is reported once.
  std::set<std::filesystem::path> reported;
  for (const auto& mount_point : current) {
    if (!last.contains(mount_point) && reported.insert(mount_point).second) {
      changes.arrived.push_back(mount_point);
    }
  }
  reported.clear();
  for (const auto& mount_point : mount_points_) {
    if (!now.contains(mount_point) && reported.insert(mount_point).second) {
      changes.removed.push_back(mount_point);
    }
  }
  mount_points_ = std::move(current);
  return changes;
}

auto CreateQtMountDirectoryWatcher(std::size_t directory_limit, MountTableReader read_mount_table,
                                   std::chrono::milliseconds poll_interval,
                                   ArrivedVolumeReader       read_arrived_volume)
    -> std::unique_ptr<MountDirectoryWatcher> {
  return std::make_unique<QtMountDirectoryWatcher>(directory_limit, std::move(read_mount_table),
                                                   poll_interval, std::move(read_arrived_volume));
}

#ifndef _WIN32
auto CreatePlatformMountDirectoryWatcher() -> std::unique_ptr<MountDirectoryWatcher> {
  return CreateQtMountDirectoryWatcher();
}
#endif

}  // namespace alcedo::ui
