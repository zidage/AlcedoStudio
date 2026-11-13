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
  if (directory == root) {
    return {};
  }
  const auto relative = directory.lexically_relative(root);
  if (relative.empty()) {
    // The directory is outside the root (a drop from two drives has no common root).
    return album_util::PathToQString(directory.lexically_normal().make_preferred());
  }
  if (relative == std::filesystem::path(".")) {
    return {};
  }
  return album_util::PathToQString(relative.lexically_normal().make_preferred());
}

/// True when every component of @p prefix starts @p path.
auto PathStartsWith(const std::filesystem::path& path, const std::filesystem::path& prefix)
    -> bool {
  auto       path_it   = path.begin();
  const auto path_end  = path.end();
  for (const auto& part : prefix) {
    if (path_it == path_end || *path_it != part) {
      return false;
    }
    ++path_it;
  }
  return true;
}

}  // namespace

auto BuildAllowedRows(std::span<const ImportFileCategory> categories, ImportCategoryMask allowed)
    -> std::vector<uint32_t> {
  return BuildAllowedRows(categories, {}, allowed);
}

auto BuildAllowedRows(std::span<const ImportFileCategory> categories,
                      std::span<const uint8_t> dropped, ImportCategoryMask allowed)
    -> std::vector<uint32_t> {
  std::vector<uint32_t> rows;
  rows.reserve(categories.size());
  for (std::size_t index = 0; index < categories.size(); ++index) {
    const bool is_dropped = index < dropped.size() && dropped[index] != 0;
    if (is_dropped || CategoryAllowed(allowed, categories[index])) {
      rows.push_back(static_cast<uint32_t>(index));
    }
  }
  return rows;
}

auto CommonParentFolder(std::span<const std::filesystem::path> sources) -> std::filesystem::path {
  if (sources.empty()) {
    return {};
  }
  std::filesystem::path common = sources.front().lexically_normal().parent_path();
  for (std::size_t index = 1; index < sources.size(); ++index) {
    const auto parent = sources[index].lexically_normal().parent_path();
    while (!common.empty() && !PathStartsWith(parent, common)) {
      const auto up = common.parent_path();
      common        = up == common ? std::filesystem::path{} : up;
    }
  }
  return common;
}

FolderImportScanModel::FolderImportScanModel(QObject* parent) : QAbstractListModel(parent) {}

FolderImportScanModel::~FolderImportScanModel() { StopWorker(); }

int FolderImportScanModel::rowCount(const QModelIndex& parent) const {
  return parent.isValid() ? 0 : static_cast<int>(rows_.size());
}

auto FolderImportScanModel::data(const QModelIndex& index, int role) const -> QVariant {
  if (!index.isValid() || index.row() < 0 ||
      static_cast<std::size_t>(index.row()) >= rows_.size()) {
    return {};
  }
  const auto  file = rows_[static_cast<std::size_t>(index.row())];
  const auto& path = file_paths_[file];
  switch (role) {
    case CategoryRole:
      return static_cast<int>(categories_[file]);
    case Qt::DisplayRole:
    case FileNameRole:
      return album_util::PathToQString(path.filename());
    case RelativeDirectoryRole:
      return RelativeDirectoryText(path.parent_path(), root_);
    case DroppedRole:
      return dropped_[file] != 0;
    default:
      return {};
  }
}

auto FolderImportScanModel::roleNames() const -> QHash<int, QByteArray> {
  return {{FileNameRole, "fileName"},
          {RelativeDirectoryRole, "relativeDirectory"},
          {CategoryRole, "category"},
          {DroppedRole, "dropped"}};
}

void FolderImportScanModel::SetAllowedCategories(int mask) {
  const auto allowed = static_cast<ImportCategoryMask>(mask & kAllImportCategories);
  if (allowed == allowed_) {
    return;
  }
  allowed_ = allowed;
  beginResetModel();
  rows_ = BuildAllowedRows(categories_, dropped_, allowed_);
  endResetModel();
  emit AllowedCategoriesChanged();
}

void FolderImportScanModel::SetCategoryAllowed(int category, bool allowed) {
  if (category < 0 || category >= static_cast<int>(ImportFileCategory::Other)) {
    return;
  }
  const auto bit = ImportCategoryBit(static_cast<ImportFileCategory>(category));
  SetAllowedCategories(allowed ? (allowed_ | bit) : (allowed_ & ~bit));
}

auto FolderImportScanModel::CategoryCounts() const -> QVariantList {
  QVariantList counts;
  for (const int count : counts_) {
    counts.push_back(count);
  }
  return counts;
}

void FolderImportScanModel::Start(const QString& folderUrlOrPath) {
  const auto root_opt = album_util::InputToPath(folderUrlOrPath);
  if (!root_opt.has_value()) {
    StopWorker();
    ClearRows();
    root_.clear();
    folder_path_.clear();
    current_directory_.clear();
    scanning_      = false;
    scan_finished_ = true;
    folder_valid_  = false;
    emit ScanStateChanged();
    return;
  }
  StartScan({root_opt.value()}, root_opt.value(), /*files_are_dropped=*/false);
}

void FolderImportScanModel::StartSources(const QStringList& urlsOrPaths) {
  std::vector<std::filesystem::path> sources;
  sources.reserve(static_cast<std::size_t>(urlsOrPaths.size()));
  for (const QString& raw : urlsOrPaths) {
    if (const auto path = album_util::InputToPath(raw); path.has_value()) {
      sources.push_back(path->lexically_normal());
    }
  }
  auto root = CommonParentFolder(sources);
  StartScan(std::move(sources), std::move(root), /*files_are_dropped=*/true);
}

void FolderImportScanModel::StartScan(std::vector<std::filesystem::path> sources,
                                      std::filesystem::path root, const bool files_are_dropped) {
  StopWorker();
  ClearRows();

  root_        = std::move(root);
  folder_path_ = root_.empty() ? QString{} : album_util::PathToQString(root_);
  current_directory_.clear();

  scanning_      = true;
  scan_finished_ = false;
  folder_valid_  = true;
  emit ScanStateChanged();

  auto scan    = std::make_shared<Scan>();
  active_scan_ = scan;
  worker_      = std::thread([this, scan, sources = std::move(sources), root = root_,
                         files_are_dropped]() {
    using Clock = std::chrono::steady_clock;

    std::vector<Entry>    found;
    std::vector<Entry>    batch;
    std::filesystem::path current_directory = root;
    auto                  last_post         = Clock::now();
    bool                  any_source_valid  = false;
    const auto            post_batch        = [&]() {
      QMetaObject::invokeMethod(
          this,
          [this, scan, entries = std::move(batch),
           directory = RelativeDirectoryText(current_directory, root)]() mutable {
            AppendBatch(scan, std::move(entries), directory);
          },
          Qt::QueuedConnection);
      batch.clear();
      last_post = Clock::now();
    };
    const auto add = [&](const std::filesystem::path& path, const bool dropped) {
      found.push_back(Entry{path, dropped});
      batch.push_back(Entry{path, dropped});
      if (batch.size() >= kScanBatchSize || Clock::now() - last_post >= kScanBatchInterval) {
        post_batch();
      }
    };

    for (const auto& source : sources) {
      if (scan->stop_requested_.load()) {
        return;
      }
      std::error_code source_ec;
      if (files_are_dropped && std::filesystem::is_regular_file(source, source_ec) && !source_ec) {
        any_source_valid  = true;
        current_directory = source.parent_path();
        add(source, true);
        continue;
      }
      source_ec.clear();
      if (!std::filesystem::is_directory(source, source_ec) || source_ec) {
        continue;
      }
      any_source_valid = true;

      std::error_code ec;
      for (auto it = std::filesystem::recursive_directory_iterator(
               source, std::filesystem::directory_options::skip_permission_denied, ec);
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
          add(it->path(), false);
        }
      }
    }
    if (scan->stop_requested_.load()) {
      return;
    }

    // Import order is sorted by path, so the order of the library elements does not depend on
    // the directory order the file system returns. A dropped file that is also inside a dropped
    // folder is one dropped entry.
    std::sort(found.begin(), found.end(),
              [](const Entry& lhs, const Entry& rhs) { return lhs.path_ < rhs.path_; });
    std::vector<Entry> unique;
    unique.reserve(found.size());
    for (auto& entry : found) {
      if (!unique.empty() && unique.back().path_ == entry.path_) {
        unique.back().dropped_ = unique.back().dropped_ || entry.dropped_;
        continue;
      }
      unique.push_back(std::move(entry));
    }
    QMetaObject::invokeMethod(
        this,
        [this, scan, sorted = std::move(unique), any_source_valid]() mutable {
          FinishScan(scan, std::move(sorted), any_source_valid);
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

auto FolderImportScanModel::TakeFilePaths(ImportPathSet* dropped_paths)
    -> std::vector<image_path_t> {
  if (!scan_finished_) {
    return {};
  }
  std::vector<image_path_t> paths;
  paths.reserve(rows_.size());
  for (const auto file : rows_) {
    if (dropped_paths != nullptr && dropped_[file] != 0) {
      dropped_paths->insert(file_paths_[file].native());
    }
    paths.push_back(std::move(file_paths_[file]));
  }
  beginResetModel();
  file_paths_.clear();
  categories_.clear();
  dropped_.clear();
  dropped_count_ = 0;
  rows_.clear();
  counts_.fill(0);
  endResetModel();
  emit AllowedCategoriesChanged();
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
  categories_.clear();
  dropped_.clear();
  dropped_count_ = 0;
  rows_.clear();
  counts_.fill(0);
  endResetModel();
  emit AllowedCategoriesChanged();
}

bool FolderImportScanModel::AddEntry(Entry entry) {
  const auto category = CategoryForPath(entry.path_);
  if (entry.dropped_) {
    ++dropped_count_;
  } else {
    ++counts_[static_cast<std::size_t>(category)];
  }
  categories_.push_back(category);
  dropped_.push_back(entry.dropped_ ? 1 : 0);
  file_paths_.push_back(std::move(entry.path_));
  return entry.dropped_ || CategoryAllowed(allowed_, category);
}

void FolderImportScanModel::AppendBatch(const std::shared_ptr<Scan>& scan,
                                        std::vector<Entry>           batch,
                                        const QString&               current_directory) {
  if (scan != active_scan_) {
    return;
  }
  if (!batch.empty()) {
    std::vector<uint32_t> new_rows;
    for (auto& entry : batch) {
      const auto index = static_cast<uint32_t>(file_paths_.size());
      if (AddEntry(std::move(entry))) {
        new_rows.push_back(index);
      }
    }
    if (!new_rows.empty()) {
      const auto first = static_cast<int>(rows_.size());
      beginInsertRows(QModelIndex(), first, first + static_cast<int>(new_rows.size()) - 1);
      rows_.insert(rows_.end(), new_rows.begin(), new_rows.end());
      endInsertRows();
      emit AllowedCategoriesChanged();
    }
  }
  current_directory_ = current_directory;
  emit ScanStateChanged();
}

void FolderImportScanModel::FinishScan(const std::shared_ptr<Scan>& scan,
                                       std::vector<Entry>           sorted_entries,
                                       const bool                   folder_valid) {
  if (scan != active_scan_) {
    return;
  }
  // The worker posted this as its last action; joining only waits for its return.
  if (worker_.joinable()) {
    worker_.join();
  }
  beginResetModel();
  file_paths_.clear();
  categories_.clear();
  dropped_.clear();
  dropped_count_ = 0;
  counts_.fill(0);
  file_paths_.reserve(sorted_entries.size());
  categories_.reserve(sorted_entries.size());
  dropped_.reserve(sorted_entries.size());
  for (auto& entry : sorted_entries) {
    AddEntry(std::move(entry));
  }
  rows_ = BuildAllowedRows(categories_, dropped_, allowed_);
  endResetModel();
  emit AllowedCategoriesChanged();
  scanning_      = false;
  scan_finished_ = true;
  folder_valid_  = folder_valid;
  current_directory_.clear();
  emit ScanStateChanged();
}

}  // namespace alcedo::ui
