//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QModelIndex>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <thread>
#include <vector>

#include "type/supported_file_type.hpp"
#include "type/type.hpp"

namespace alcedo::ui {

/// Indices of the files whose category is in @p allowed, in file order. O(n): a checkbox change
/// rebuilds the list view with this and never rescans (raster_image_input_plan.md, section 9.2).
[[nodiscard]] auto          BuildAllowedRows(std::span<const ImportFileCategory> categories,
                                             ImportCategoryMask allowed) -> std::vector<uint32_t>;
/// BuildAllowedRows with one dropped flag per file (non-zero = dropped). A dropped file is a row
/// whatever its category; the other files follow @p allowed.
[[nodiscard]] auto          BuildAllowedRows(std::span<const ImportFileCategory> categories,
                                             std::span<const uint8_t>            dropped,
                                             ImportCategoryMask allowed) -> std::vector<uint32_t>;
/// Deepest folder that holds every entry of @p sources (lexical, no file system access): the
/// common prefix of their parent folders. Empty when @p sources is empty or the entries share
/// no root (two drives).
[[nodiscard]] auto          CommonParentFolder(std::span<const std::filesystem::path> sources)
    -> std::filesystem::path;

/// Files found by a recursive scan of the folder chosen for "Import From Folder", or of the files
/// and folders of a Library drop (issue #757).
///
/// Start() and StartSources() return at once: a worker thread walks the folder trees and appends
/// the regular files it finds in batches, so the confirmation dialog shows the list and a live
/// count while a large tree (tens of thousands of files) is still being read. When the walk ends,
/// the rows are replaced by the same files sorted by path, which is the import order. The model
/// owns the scanned paths; the import takes them with TakeFilePaths() instead of a round trip
/// through QML.
///
/// A file source of StartSources() is a dropped file: it is a row whatever its category, as in
/// the Import Images path where the user picked the file. The files found inside folder sources
/// follow the allowed categories. A file that is a dropped file and also inside a dropped folder
/// is one dropped row.
class FolderImportScanModel final : public QAbstractListModel {
  Q_OBJECT
  Q_PROPERTY(bool scanning READ Scanning NOTIFY ScanStateChanged)
  Q_PROPERTY(bool scanFinished READ ScanFinished NOTIFY ScanStateChanged)
  Q_PROPERTY(bool folderValid READ FolderValid NOTIFY ScanStateChanged)
  Q_PROPERTY(int fileCount READ FileCount NOTIFY ScanStateChanged)
  Q_PROPERTY(QString folderPath READ FolderPath NOTIFY ScanStateChanged)
  Q_PROPERTY(QString currentDirectory READ CurrentDirectory NOTIFY ScanStateChanged)
  /// Mask of ImportFileCategory bits that the list shows and import accepts. Other is never set.
  Q_PROPERTY(int allowedCategories READ AllowedCategories WRITE SetAllowedCategories NOTIFY
                 AllowedCategoriesChanged)
  /// Count per ImportFileCategory of the files found inside folders, in enum order (Raw, Jpeg,
  /// Tiff, Png, OpenExr, Other). Dropped files are not in these counts.
  Q_PROPERTY(QVariantList categoryCounts READ CategoryCounts NOTIFY ScanStateChanged)
  /// Rows of the list and the files that import takes: the dropped files and the folder files in
  /// allowed categories.
  Q_PROPERTY(int allowedFileCount READ AllowedFileCount NOTIFY AllowedCategoriesChanged)
  /// Dropped files of the scan (StartSources file sources).
  Q_PROPERTY(int droppedFileCount READ DroppedFileCount NOTIFY ScanStateChanged)

 public:
  enum Role {
    FileNameRole = Qt::UserRole + 1,
    /// Folder of the file relative to the scan root (the scanned folder, or the common parent
    /// folder of the drop); empty for files directly inside it.
    RelativeDirectoryRole,
    /// ImportFileCategory of the file, as an int.
    CategoryRole,
    /// True for a dropped file.
    DroppedRole,
  };

  explicit FolderImportScanModel(QObject* parent = nullptr);
  ~FolderImportScanModel() override;

  [[nodiscard]] int  rowCount(const QModelIndex& parent = QModelIndex()) const override;
  [[nodiscard]] auto data(const QModelIndex& index, int role = Qt::DisplayRole) const
      -> QVariant override;
  [[nodiscard]] auto roleNames() const -> QHash<int, QByteArray> override;

  /// Stop any running scan, clear the rows, and scan @p folderUrlOrPath on a worker thread.
  Q_INVOKABLE void   Start(const QString& folderUrlOrPath);
  /// Stop any running scan, clear the rows, and scan the files and folders of a drop on a worker
  /// thread. The scan root (folderPath, relative directories) is the common parent folder of the
  /// sources. Entries that are neither a regular file nor a folder are skipped; folderValid is
  /// false when no source is either.
  Q_INVOKABLE void   StartSources(const QStringList& urlsOrPaths);
  /// Stop a running scan and clear the rows.
  Q_INVOKABLE void   Cancel();

  /// Move the rows' paths (sorted) out of a finished scan and clear the model. Empty while a
  /// scan is running. @p dropped_paths, when set, receives the native paths of the dropped
  /// files among them: the import accepts those in every category (ImportOptions).
  auto               TakeFilePaths(ImportPathSet* dropped_paths = nullptr)
      -> std::vector<image_path_t>;

  /// Allow or exclude one category. Other cannot be allowed.
  Q_INVOKABLE void   SetCategoryAllowed(int category, bool allowed);

  [[nodiscard]] bool Scanning() const { return scanning_; }
  [[nodiscard]] bool ScanFinished() const { return scan_finished_; }
  [[nodiscard]] bool FolderValid() const { return folder_valid_; }
  [[nodiscard]] int  FileCount() const { return static_cast<int>(file_paths_.size()); }
  [[nodiscard]] int  AllowedCategories() const { return allowed_; }
  void               SetAllowedCategories(int mask);
  [[nodiscard]] auto CategoryCounts() const -> QVariantList;
  [[nodiscard]] int  AllowedFileCount() const { return static_cast<int>(rows_.size()); }
  [[nodiscard]] int  DroppedFileCount() const { return dropped_count_; }
  [[nodiscard]] auto FolderPath() const -> QString { return folder_path_; }
  [[nodiscard]] auto CurrentDirectory() const -> QString { return current_directory_; }

 signals:
  void ScanStateChanged();
  void AllowedCategoriesChanged();

 private:
  /// Stop request of one scan. Queued batches carry their scan; a batch from a scan that is no
  /// longer active_scan_ is dropped. Cancel() followed by Start() can leave batches of the
  /// cancelled scan in the event queue, and without this check they would be appended to the
  /// new folder's rows.
  struct Scan {
    std::atomic<bool> stop_requested_{false};
  };

  /// One file the worker found.
  struct Entry {
    image_path_t path_{};
    bool         dropped_ = false;
  };

  /// Stop any running scan, clear the rows, and walk @p sources on the worker thread. A source
  /// that is a folder is walked recursively; a source that is a regular file is a dropped file
  /// when @p files_are_dropped.
  void StartScan(std::vector<std::filesystem::path> sources, std::filesystem::path root,
                 bool files_are_dropped);
  void StopWorker();
  void ClearRows();
  /// Store @p entry and count it; returns true when it is a row under allowed_.
  bool AddEntry(Entry entry);
  void AppendBatch(const std::shared_ptr<Scan>& scan, std::vector<Entry> batch,
                   const QString& current_directory);
  void FinishScan(const std::shared_ptr<Scan>& scan, std::vector<Entry> sorted_entries,
                  bool folder_valid);

  std::vector<image_path_t>       file_paths_{};
  /// Category of each entry of file_paths_.
  std::vector<ImportFileCategory> categories_{};
  /// Dropped flag (1 = dropped file) of each entry of file_paths_.
  std::vector<uint8_t>            dropped_{};
  int                             dropped_count_ = 0;
  /// Indices into file_paths_ of the rows the list shows.
  std::vector<uint32_t>           rows_{};
  std::array<int, 6>              counts_{};
  ImportCategoryMask              allowed_ = kAllImportCategories;
  std::filesystem::path     root_{};
  std::shared_ptr<Scan>     active_scan_{};
  std::thread               worker_{};
  bool                      scanning_      = false;
  bool                      scan_finished_ = false;
  bool                      folder_valid_  = true;
  QString                   folder_path_{};
  QString                   current_directory_{};
};

}  // namespace alcedo::ui
