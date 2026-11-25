//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "app/adjustment_transfer_types.hpp"
#include "app/preset_file.hpp"
#include "app/preset_library_scan.hpp"
#include "concurrency/thread_pool.hpp"
#include "edit/operators/models/lut_reference.hpp"

namespace alcedo {

/// Number of parsed packages that the preset library keeps in memory (edit presets plan 6.2).
inline constexpr std::size_t kPresetPackageCacheCapacity = 64;

/// Persistent preferences of the preset library.
class PresetLibraryPreferences {
 public:
  virtual ~PresetLibraryPreferences()                                                 = default;
  /// The selected folder, or std::nullopt when the user has not chosen one.
  [[nodiscard]] virtual auto LoadRoot() const -> std::optional<std::filesystem::path> = 0;
  /// Persist the selected folder. Returns false when the value cannot be stored.
  [[nodiscard]] virtual auto SaveRoot(const std::filesystem::path& root) -> bool      = 0;
};

/// QSettings-backed preferences (`presets/libraryRoot`).
class SettingsPresetLibraryPreferences final : public PresetLibraryPreferences {
 public:
  [[nodiscard]] auto LoadRoot() const -> std::optional<std::filesystem::path> override;
  [[nodiscard]] auto SaveRoot(const std::filesystem::path& root) -> bool override;
};

/// `~/.alcedo/presets`, resolved from the current user's home directory (plan D3).
[[nodiscard]] auto DefaultPresetLibraryRoot() -> std::filesystem::path;

struct PresetLibraryServiceOptions {
  std::unique_ptr<PresetLibraryPreferences> preferences;
  /// Folder used when the preferences hold none. Created on Start() if absent.
  std::filesystem::path                     default_root = DefaultPresetLibraryRoot();
  /// Reference of the LUT library entry at an absolute path (LutLibraryService::ReferenceForPath),
  /// or std::nullopt when the LUT library does not list the file. Called on the owner thread by
  /// Save. When empty, each LUT file path refuses a save.
  std::function<std::optional<LutReference>(const std::filesystem::path&)> lut_reference_for_path;
  /// Move a file to the system trash. Returns false and sets the message on failure. Called on
  /// the worker thread. Defaults to QFile::moveToTrash.
  std::function<bool(const std::filesystem::path&, std::string*)>          move_to_trash;
};

/// A parsed preset package, or the reason why it is not available.
struct PresetPackageResult {
  std::shared_ptr<const AdjustmentTransferPackage> package;
  PresetFileRule                                   rule = PresetFileRule::kNone;
  std::string                                      message;

  [[nodiscard]] auto                               Ok() const -> bool { return package != nullptr; }
};

/**
 * @brief The single owner of the preset folder: the folder choice, the inventory of its preset
 *        files, the package cache, and the operations that change preset files (edit presets
 *        plan 6.1).
 *
 * Thread affinity: every public method runs on the thread that owns this object (the GUI
 * thread). Scans and file operations run in request order on one worker thread; a request
 * during an operation waits in the worker queue. The worker owns the folder that the queued
 * operations use and the preferences; the owner thread owns the published folder, the
 * inventory, and the package cache. Each completion publishes on the owner thread.
 *
 * The service has no file watcher (plan D9). It scans at Start, at Refresh, at UseRoot, and after
 * each of its own writes.
 */
class PresetLibraryService final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString rootPath READ root_path NOTIFY RootChanged)
  Q_PROPERTY(bool busy READ busy NOTIFY OperationStateChanged)
  Q_PROPERTY(QString lastError READ last_error NOTIFY OperationStateChanged)
  Q_PROPERTY(int entryCount READ entry_count NOTIFY InventoryChanged)

 public:
  enum class Operation { kNone, kScan, kUseRoot, kSave, kAdd, kRename, kDelete, kExport };
  Q_ENUM(Operation)

  enum class Status {
    kOk,
    /// The folder is not available.
    kRootUnavailable,
    /// The request names no listed preset, or a name that cannot be a preset name.
    kInvalidRequest,
    /// A preset file refused a rule of ReadPresetFile, or a package refused a save rule.
    kRefused,
    /// A file with the requested name exists.
    kConflict,
    kIoError,
    kPreferenceError,
  };
  Q_ENUM(Status)

  /// Outcome of the most recent completed operation.
  struct OperationResult {
    Operation                operation = Operation::kNone;
    Status                   status    = Status::kOk;
    std::string              message;
    /// Save, Add, and Rename: ids of the new or renamed preset files.
    std::vector<std::string> affected_ids;
    /// The request of the operation (LastSubmittedRequest after its call); 0 for a refusal on
    /// the calling thread.
    std::uint64_t            request = 0;
  };

  explicit PresetLibraryService(PresetLibraryServiceOptions options, QObject* parent = nullptr);
  ~PresetLibraryService() override;

  PresetLibraryService(const PresetLibraryService&)            = delete;
  PresetLibraryService& operator=(const PresetLibraryService&) = delete;

  /// Create the default folder when no folder is chosen and it is absent, then scan.
  void                  Start();
  /// Drop queued operations and join the worker. Idempotent.
  void                  Shutdown();

  [[nodiscard]] auto    Root() const -> const std::filesystem::path& { return root_; }
  [[nodiscard]] auto    root_path() const -> QString;
  [[nodiscard]] auto    busy() const -> bool { return pending_operations_ > 0; }
  [[nodiscard]] auto    last_error() const -> QString { return last_error_; }
  [[nodiscard]] auto    entry_count() const -> int { return static_cast<int>(Entries().size()); }
  [[nodiscard]] auto    LastResult() const -> const OperationResult& { return last_result_; }
  /**
   * @brief The request of the last operation that this thread queued, from 1.
   *
   * The worker runs the operations in request order, so a caller that queues one (an automation
   * save) can find its own completion among those of other callers: the OperationFinished whose
   * LastResult().request is this value.
   */
  [[nodiscard]] auto    LastSubmittedRequest() const -> std::uint64_t { return last_request_; }

  /// The published entries in id order. Valid until the next InventoryChanged.
  [[nodiscard]] auto    Entries() const -> const std::vector<PresetEntry>& {
    return inventory_.entries;
  }
  /// The published entry @p id, or nullptr. Valid until the next InventoryChanged.
  [[nodiscard]] auto FindEntry(std::string_view id) const -> const PresetEntry*;
  /// Error of the last published scan; empty when the scan read the complete folder.
  [[nodiscard]] auto InventoryError() const -> const std::string& { return inventory_.error; }

  /**
   * @brief The parsed package of the listed preset @p id (plan 6.2).
   *
   * Reads the file size and the write time. When both are equal to the values of the cached
   * read, returns the cached package; otherwise reads the file again with ReadPresetFile and
   * caches the result. The cache keeps kPresetPackageCacheCapacity packages, least recently used
   * first out. Reads the file on the calling (owner) thread. An id that the inventory does not
   * list is refused without a file read.
   */
  [[nodiscard]] auto PackageFor(std::string_view id) -> PresetPackageResult;
  /// Number of preset file reads that PackageFor made. A diagnostic of the cache.
  [[nodiscard]] auto PackageFileReadCount() const -> std::size_t { return package_file_reads_; }

  /// Scan the folder again.
  void               Refresh();
  /**
   * @brief Use @p root as the preset folder. Moves no file.
   *
   * The worker scans @p root and stores the preference before the folder changes; a failure
   * keeps the current folder. Operations queued after this one use @p root.
   */
  void               UseRoot(const std::filesystem::path& root);
  /**
   * @brief Save @p package as a new preset named @p name in the folder.
   *
   * On the calling thread: checks the name, replaces each LMT `cube_path` of a LUT library file
   * with the reference of its entry (lut_reference_for_path), and applies CheckPresetReferences.
   * A LUT file outside the LUT library or a lens database path refuses the save (kRefused).
   * On the worker: a name that exists gets ` 2`, ` 3`, and so on; WritePresetFile never
   * replaces a file. The new id is the only affected id.
   *
   * @return kOk when the save is queued, else the refusal; a refusal is also the last result.
   */
  auto             Save(const AdjustmentTransferPackage& package, std::string_view name) -> Status;
  /**
   * @brief Write @p package as a preset file at @p path, outside the preset list (issue #1124).
   *
   * The same checks on the calling thread as Save. On the worker the file is written with
   * PresetFileWriteMode::kReplace: the system save dialog has asked before it replaces a file.
   * The folder is scanned again when @p path is in it. No id is affected.
   *
   * @return kOk when the export is queued, else the refusal; a refusal is also the last result.
   */
  auto             Export(const AdjustmentTransferPackage& package,
                          const std::filesystem::path&     path) -> Status;
  /**
   * @brief Copy preset files into the folder.
   *
   * Each source is read with ReadPresetFile; the accepted text, not the source file, is written.
   * A source that fails a rule is not written and its reason is in the result message; the other
   * sources are added. The name is the source file name without the extension; a name that
   * exists gets a number. The result is kRefused when one or more sources failed.
   */
  void             Add(std::vector<std::filesystem::path> sources);
  /// Rename the listed preset @p id to @p name in its folder. An existing name is a conflict.
  auto             Rename(std::string_view id, std::string_view name) -> Status;
  /// Move the listed preset @p id to the system trash (plan D8).
  auto             Delete(std::string_view id) -> Status;

  Q_INVOKABLE void refresh() { Refresh(); }
  /// @p folder accepts a native path and a local-file URL (folder dialogs).
  Q_INVOKABLE void useRoot(const QString& folder);

 signals:
  void InventoryChanged();
  void RootChanged();
  void OperationStateChanged();
  /// An operation completed; details are in LastResult().
  void OperationFinished(alcedo::PresetLibraryService::Operation operation,
                         alcedo::PresetLibraryService::Status    status);

 private:
  using Completion = std::function<void()>;

  /// The result of one worker operation, published on the owner thread.
  struct WorkerResult {
    OperationResult                       result;
    std::optional<PresetLibraryInventory> inventory;
    std::optional<std::filesystem::path>  root;
  };

  /// Queue @p work on the worker; its result is published on the owner thread.
  void               Submit(Operation operation, std::function<WorkerResult()> work);
  void               Publish(WorkerResult worker_result);
  /// Record a refusal on the calling thread and announce it.
  auto               Refuse(Operation operation, Status status, std::string message) -> Status;
  /**
   * @brief The written preset of @p package for Save and Export, on the calling thread.
   *
   * Library references in place of LUT paths, the fingerprint, and CheckPresetReferences. Null
   * after a refusal of @p operation.
   */
  auto               PresetForWrite(Operation operation, const AdjustmentTransferPackage& package)
      -> std::shared_ptr<const AdjustmentTransferPackage>;
  /// Worker: scan the worker folder into @p worker_result.
  void               ScanWorkerRoot(WorkerResult* worker_result) const;
  /// Worker: the first free file name for @p name in @p folder, or the reason why none exists.
  [[nodiscard]] auto FreePresetName(const std::filesystem::path& folder, std::string_view name,
                                    std::string* error) const -> std::optional<std::string>;
  /// Drop cached packages of entries that the published inventory no longer lists.
  void               PruneCache();

  struct CachedPackage {
    std::string                                      id;
    std::uintmax_t                                   file_size = 0;
    std::filesystem::file_time_type                  write_time{};
    std::shared_ptr<const AdjustmentTransferPackage> package;
  };

  PresetLibraryServiceOptions options_;
  /// Published folder and inventory (owner thread).
  std::filesystem::path       root_;
  PresetLibraryInventory      inventory_;
  /// Most recently used first (owner thread).
  std::list<CachedPackage>    cache_;
  std::size_t                 package_file_reads_ = 0;
  std::size_t                 pending_operations_ = 0;
  std::uint64_t               last_request_       = 0;
  OperationResult             last_result_;
  QString                     last_error_;
  bool                        shut_down_ = false;
  /// Folder of the queued operations. Written and read on the worker thread only, after the
  /// constructor.
  std::filesystem::path       worker_root_;
  /// One operation at a time, in request order; Shutdown joins before owner state is destroyed.
  ThreadPool                  worker_{1};
};

}  // namespace alcedo
