//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <set>
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
                          std::chrono::milliseconds poll_interval)
      : directory_limit_(directory_limit),
        read_mount_table_(read_mount_table ? std::move(read_mount_table)
                                           : MountTableReader(&ListMountPointPaths)) {
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

  void SetVolumeCallbacks(VolumeRemovedCallback removed, VolumeArrivedCallback arrived) override {
    on_volume_removed_ = std::move(removed);
    on_volume_arrived_ = std::move(arrived);
    if (!on_volume_removed_ && !on_volume_arrived_) {
      poll_timer_.stop();
      return;
    }
    // The table of this call is the state that the first poll compares with.
    mount_points_ = ReadMountTable();
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
  auto ReadMountTable() const -> std::set<std::filesystem::path> {
    const auto paths = read_mount_table_();
    return {paths.begin(), paths.end()};
  }

  /// Compare the mount table with the last read. A mount point that is gone removes the watch of
  /// each mount whose root is at or below it; a new mount point is an arrival.
  void PollMountTable() {
    auto                    current = ReadMountTable();
    std::vector<mount_id_t> removed_mounts;
    bool                    arrived = false;
    for (const auto& mount_point : mount_points_) {
      if (current.contains(mount_point)) {
        continue;
      }
      for (const auto& [mount_id, root] : root_of_mount_) {
        if (IsPathAtOrBelow(root, mount_point)) {
          removed_mounts.push_back(mount_id);
        }
      }
    }
    for (const auto& mount_point : current) {
      if (!mount_points_.contains(mount_point)) {
        arrived = true;
        break;
      }
    }
    mount_points_ = std::move(current);

    for (const auto mount_id : removed_mounts) {
      Unwatch(mount_id);
    }
    // A callback can change the watches; the ids are a copy.
    for (const auto mount_id : removed_mounts) {
      if (on_volume_removed_) {
        on_volume_removed_(mount_id);
      }
    }
    if (arrived && on_volume_arrived_) {
      on_volume_arrived_();
    }
  }

  std::size_t                                           directory_limit_;
  MountTableReader                                      read_mount_table_;
  QFileSystemWatcher                                    watcher_;
  QTimer                                                poll_timer_;
  ChangeCallback                                        on_change_;
  VolumeRemovedCallback                                 on_volume_removed_;
  VolumeArrivedCallback                                 on_volume_arrived_;
  std::unordered_map<QString, mount_id_t>               mount_of_path_;
  std::unordered_map<mount_id_t, QStringList>           paths_of_mount_;
  std::unordered_map<mount_id_t, std::filesystem::path> root_of_mount_;
  std::set<std::filesystem::path>                       mount_points_;
  std::size_t                                           watched_count_ = 0;
};

}  // namespace

auto CreateQtMountDirectoryWatcher(std::size_t directory_limit, MountTableReader read_mount_table,
                                   std::chrono::milliseconds poll_interval)
    -> std::unique_ptr<MountDirectoryWatcher> {
  return std::make_unique<QtMountDirectoryWatcher>(directory_limit, std::move(read_mount_table),
                                                   poll_interval);
}

#ifndef _WIN32
auto CreatePlatformMountDirectoryWatcher() -> std::unique_ptr<MountDirectoryWatcher> {
  return CreateQtMountDirectoryWatcher();
}
#endif

}  // namespace alcedo::ui
