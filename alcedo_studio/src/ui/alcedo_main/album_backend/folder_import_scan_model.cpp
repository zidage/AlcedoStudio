//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "ui/alcedo_main/album_backend/folder_import_scan_model.hpp"

#include <QMetaObject>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <system_error>
#include <utility>

#include "ui/alcedo_main/album_backend/path_utils.hpp"

namespace alcedo::ui {

namespace {

/// The worker hands found files to the model after this many files or this much time, whichever
/// comes first, so a large tree fills the list steadily without one queued call per file.
constexpr std::size_t kScanBatchSize        = 512;
constexpr auto        kScanBatchInterval    = std::chrono::milliseconds(100);

auto RelativeDirectoryText(const std::filesystem::path& directory,
                           const std::filesystem::path& root) -> QString {
  const auto relative = directory.lexically_relative(root);
  if (relative.empty() || relative == std::filesystem::path(".")) {
    return {};
  }
  return album_util::PathToQString(relative.lexically_normal().make_preferred());
}

}  // namespace

FolderImportScanModel::FolderImportScanModel(QObject* parent) : QAbstractListModel(parent) {}

FolderImportScanModel::~FolderImportScanModel() { StopWorker(); }

int FolderImportScanModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(file_paths_.size());
}

auto FolderImportScanModel::data(const QModelIndex& index, int role) const -> QVariant {
  if (!index.isValid() || index.row() < 0 ||
      static_cast<std::size_t>(index.row()) >= file_paths_.size()) {
    return {};
  }
  const auto& path = file_paths_[static_cast<std::size_t>(index.row())];
  switch (role) {
    case Qt::DisplayRole:
    case FileNameRole:
      return album_util::PathToQString(path.filename());
    case RelativeDirectoryRole:
      return RelativeDirectoryText(path.parent_path(), root_);
    default:
      return {};
  }
}

auto FolderImportScanModel::roleNames() const -> QHash<int, QByteArray> {
  return {{FileNameRole, "fileName"}, {RelativeDirectoryRole, "relativeDirectory"}};
}

void FolderImportScanModel::Start(const QString& folderUrlOrPath) {
  StopWorker();
  ClearRows();

  const auto root_opt = album_util::InputToPath(folderUrlOrPath);
  root_               = root_opt.value_or(std::filesystem::path{});
  folder_path_        = root_opt.has_value() ? album_util::PathToQString(root_) : QString{};
  current_directory_.clear();
  if (!root_opt.has_value()) {
    scanning_      = false;
    scan_finished_ = true;
    folder_valid_  = false;
    emit ScanStateChanged();
    return;
  }

  scanning_      = true;
  scan_finished_ = false;
  folder_valid_  = true;
  emit ScanStateChanged();

  auto scan    = std::make_shared<Scan>();
  active_scan_ = scan;
  worker_      = std::thread([this, scan, root = root_]() {
    using Clock = std::chrono::steady_clock;

    std::error_code root_ec;
    if (!std::filesystem::is_directory(root, root_ec) || root_ec) {
      QMetaObject::invokeMethod(
          this, [this, scan]() { FinishScan(scan, {}, false); }, Qt::QueuedConnection);
      return;
    }

    std::vector<image_path_t> found_paths;
    std::vector<image_path_t> batch;
    std::filesystem::path     current_directory = root;
    auto                      last_post         = Clock::now();
    const auto                post_batch        = [&]() {
      QMetaObject::invokeMethod(
          this,
          [this, scan, files = std::move(batch),
           directory = RelativeDirectoryText(current_directory, root)]() mutable {
            AppendBatch(scan, std::move(files), directory);
          },
          Qt::QueuedConnection);
      batch.clear();
      last_post = Clock::now();
    };

    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             root, std::filesystem::directory_options::skip_permission_denied, ec);
         it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
      if (scan->stop_requested_.load()) {
        return;
      }
      if (ec) {
        ec.clear();
        continue;
      }
      current_directory = it->path().parent_path();
      std::error_code file_ec;
      if (it->is_regular_file(file_ec) && !file_ec) {
        found_paths.push_back(it->path());
        batch.push_back(it->path());
      }
      if (batch.size() >= kScanBatchSize || Clock::now() - last_post >= kScanBatchInterval) {
        post_batch();
      }
    }
    if (scan->stop_requested_.load()) {
      return;
    }

    // Import order is sorted by path, so the order of the library elements does not depend on
    // the directory order the file system returns.
    std::sort(found_paths.begin(), found_paths.end());
    QMetaObject::invokeMethod(
        this,
        [this, scan, sorted_paths = std::move(found_paths)]() mutable {
          FinishScan(scan, std::move(sorted_paths), true);
        },
        Qt::QueuedConnection);
  });
}

void FolderImportScanModel::Cancel() {
  StopWorker();
  ClearRows();
  scanning_      = false;
  scan_finished_ = false;
  folder_valid_  = true;
  folder_path_.clear();
  current_directory_.clear();
  emit ScanStateChanged();
}

auto FolderImportScanModel::TakeFilePaths() -> std::vector<image_path_t> {
  if (!scan_finished_) {
    return {};
  }
  beginResetModel();
  auto paths = std::move(file_paths_);
  file_paths_.clear();
  endResetModel();
  scan_finished_ = false;
  folder_path_.clear();
  emit ScanStateChanged();
  return paths;
}

void FolderImportScanModel::StopWorker() {
  if (active_scan_) {
    active_scan_->stop_requested_.store(true);
    active_scan_.reset();
  }
  if (worker_.joinable()) {
    worker_.join();
  }
}

void FolderImportScanModel::ClearRows() {
  if (file_paths_.empty()) {
    return;
  }
  beginResetModel();
  file_paths_.clear();
  endResetModel();
}

void FolderImportScanModel::AppendBatch(const std::shared_ptr<Scan>& scan,
                                        std::vector<image_path_t>    batch,
                                        const QString&               current_directory) {
  if (scan != active_scan_) {
    return;
  }
  if (!batch.empty()) {
    const auto first = static_cast<int>(file_paths_.size());
    beginInsertRows(QModelIndex(), first, first + static_cast<int>(batch.size()) - 1);
    file_paths_.insert(file_paths_.end(), std::make_move_iterator(batch.begin()),
                       std::make_move_iterator(batch.end()));
    endInsertRows();
  }
  current_directory_ = current_directory;
  emit ScanStateChanged();
}

void FolderImportScanModel::FinishScan(const std::shared_ptr<Scan>& scan,
                                       std::vector<image_path_t>    sorted_paths,
                                       const bool                   folder_valid) {
  if (scan != active_scan_) {
    return;
  }
  // The worker posted this as its last action; joining only waits for its return.
  if (worker_.joinable()) {
    worker_.join();
  }
  beginResetModel();
  file_paths_ = std::move(sorted_paths);
  endResetModel();
  scanning_      = false;
  scan_finished_ = true;
  folder_valid_  = folder_valid;
  current_directory_.clear();
  emit ScanStateChanged();
}

}  // namespace alcedo::ui
