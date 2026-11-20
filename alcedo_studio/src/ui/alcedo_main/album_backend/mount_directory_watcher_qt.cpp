//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QStringList>
#include <unordered_map>
#include <utility>

#include "ui/alcedo_main/album_backend/mount_directory_watcher.hpp"
#include "ui/alcedo_main/i18n.hpp"

namespace alcedo::ui {
namespace {

/// QFileSystemWatcher on the root directory and on each listed directory of each mount.
class QtMountDirectoryWatcher final : public MountDirectoryWatcher {
 public:
  explicit QtMountDirectoryWatcher(std::size_t directory_limit)
      : directory_limit_(directory_limit) {
    QObject::connect(&watcher_, &QFileSystemWatcher::directoryChanged, &watcher_,
                     [this](const QString& path) {
                       const auto it = mount_of_path_.find(path);
                       if (it != mount_of_path_.end() && on_change_) {
                         on_change_(it->second);
                       }
                     });
  }

  void SetChangeCallback(ChangeCallback callback) override { on_change_ = std::move(callback); }

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
  }

  void UnwatchAll() override {
    while (!paths_of_mount_.empty()) {
      Unwatch(paths_of_mount_.begin()->first);
    }
  }

 private:
  std::size_t                                 directory_limit_;
  QFileSystemWatcher                          watcher_;
  ChangeCallback                              on_change_;
  std::unordered_map<QString, mount_id_t>     mount_of_path_;
  std::unordered_map<mount_id_t, QStringList> paths_of_mount_;
  std::size_t                                 watched_count_ = 0;
};

}  // namespace

auto CreateQtMountDirectoryWatcher(std::size_t directory_limit)
    -> std::unique_ptr<MountDirectoryWatcher> {
  return std::make_unique<QtMountDirectoryWatcher>(directory_limit);
}

#ifndef _WIN32
auto CreatePlatformMountDirectoryWatcher() -> std::unique_ptr<MountDirectoryWatcher> {
  return CreateQtMountDirectoryWatcher();
}
#endif

}  // namespace alcedo::ui
