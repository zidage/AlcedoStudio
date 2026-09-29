//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "app/lut_library_inventory.hpp"
#include "app/lut_library_migration.hpp"
#include "utils/lut/lut_library_scan.hpp"

namespace alcedo {

/// Persistent preferences the LUT library reads outside its root.
class LutLibraryPreferences {
 public:
  virtual ~LutLibraryPreferences()                                                    = default;
  /// The selected root, or std::nullopt when the user has not chosen one.
  [[nodiscard]] virtual auto LoadRoot() const -> std::optional<std::filesystem::path> = 0;
  /// Persist the selected root. This write is the commit point of a root change.
  /// Returns false when the value cannot be stored.
  [[nodiscard]] virtual auto SaveRoot(const std::filesystem::path& root) -> bool      = 0;
  /// Absolute favorite paths from the previous LUT panel (`editor/lutPanel/favoritePaths`).
  [[nodiscard]] virtual auto LoadLegacyFavoritePaths() const -> QStringList           = 0;
  virtual void               SaveLegacyFavoritePaths(const QStringList& paths)        = 0;
};

/// QSettings-backed preferences (`lut/libraryRoot`, `editor/lutPanel/favoritePaths`).
class SettingsLutLibraryPreferences final : public LutLibraryPreferences {
 public:
  [[nodiscard]] auto LoadRoot() const -> std::optional<std::filesystem::path> override;
  [[nodiscard]] auto SaveRoot(const std::filesystem::path& root) -> bool override;
  [[nodiscard]] auto LoadLegacyFavoritePaths() const -> QStringList override;
  void               SaveLegacyFavoritePaths(const QStringList& paths) override;
};

/// `~/.alcedo/luts`, resolved from the current user's home directory.
[[nodiscard]] auto DefaultLutLibraryRoot() -> std::filesystem::path;

/// Native local-file URL for @p directory. Spaces, `#`, `%`, and non-ASCII
/// characters stay part of the path instead of being read as URL syntax.
[[nodiscard]] auto LutLibraryDirectoryUrl(const std::filesystem::path& directory) -> QUrl;

struct LutLibraryServiceOptions {
  std::unique_ptr<LutLibraryPreferences> preferences;
  /// Root used when the preferences hold none. Created on Start() if absent.
  std::filesystem::path                  default_root    = DefaultLutLibraryRoot();
  LutLibraryFileOperations               file_operations = LutLibraryFileOperations::Default();
  /// Dispatch a folder URL to the operating system; returns false on rejection.
  /// Defaults to QDesktopServices::openUrl.
  std::function<bool(const QUrl&)>       open_url;
  /// Parallel scan workers; 0 selects the hardware thread count, capped at 8.
  unsigned                               scan_worker_count = 0;
};

/// The single owner of the LUT library: root choice, the published inventory,
/// favorites, and the one running library operation
/// (docs/roadmap/alcedo_studio/ui/lut_library_and_package_management_plan.md 5.1).
///
/// Thread affinity: every public method runs on the thread that owns this
/// object (the GUI thread). Scans, imports, root loads, migration, and source
/// cleanup run one at a time on a worker thread and never touch owner state;
/// their results are published on the owner thread after persistence succeeds.
/// A running worker may read `inventory_` and `user_state_` because they change
/// only on the owner thread when an operation completes, and root-changing
/// operations reject favorite changes while they run.
class LutLibraryService final : public QObject {
  Q_OBJECT
  Q_PROPERTY(QString rootPath READ root_path NOTIFY RootChanged)
  Q_PROPERTY(bool busy READ busy NOTIFY OperationStateChanged)
  Q_PROPERTY(bool inventoryComplete READ inventory_complete NOTIFY InventoryChanged)
  Q_PROPERTY(QString lastError READ last_error NOTIFY OperationStateChanged)

 public:
  enum class Operation { kNone, kLoad, kRefresh, kImport, kUseRoot, kMigrateRoot, kSourceCleanup };
  Q_ENUM(Operation)

  enum class Status {
    kOk,
    kBusy,
    kRootUnavailable,
    kInvalidRequest,
    kConflict,
    kIoError,
    kPersistenceError,
    kPreferenceError,
    kCanceled
  };
  Q_ENUM(Status)

  /// Outcome of the most recent completed operation.
  struct OperationResult {
    Operation                operation = Operation::kNone;
    Status                   status    = Status::kOk;
    std::string              message;
    /// Relative paths whose entries were added, removed, or changed.
    std::vector<std::string> affected_paths;
    /// Migration source files kept because they changed after copying.
    std::vector<std::string> kept_source_paths;
  };

  enum class LocateStatus { kFound, kMissing, kNotInInventory };
  struct Location {
    LocateStatus          status = LocateStatus::kNotInInventory;
    std::filesystem::path absolute_path;
  };

  explicit LutLibraryService(LutLibraryServiceOptions options, QObject* parent = nullptr);
  ~LutLibraryService() override;

  LutLibraryService(const LutLibraryService&)            = delete;
  LutLibraryService& operator=(const LutLibraryService&) = delete;

  /// Load the persisted inventory of the selected root without network access.
  /// A missing or invalid inventory is rebuilt from local files. Afterwards,
  /// resumes an interrupted migration cleanup and converts legacy favorites.
  void               Start();
  /// Stop the running operation and join the worker. Idempotent.
  void               Shutdown();

  [[nodiscard]] auto Root() const -> const std::filesystem::path& { return root_; }
  [[nodiscard]] auto root_path() const -> QString;
  [[nodiscard]] auto busy() const -> bool { return operation_ != Operation::kNone; }
  [[nodiscard]] auto inventory_complete() const -> bool { return inventory_.Complete(); }
  [[nodiscard]] auto last_error() const -> QString { return last_error_; }
  [[nodiscard]] auto CurrentOperation() const -> Operation { return operation_; }
  [[nodiscard]] auto LastResult() const -> const OperationResult& { return last_result_; }

  /// Scoped const read of one entry. Returns false when the path is not listed.
  /// Do not retain the reference after @p visitor returns.
  auto               ReadEntry(std::string_view                                   relative_path,
                               const std::function<void(const LutLibraryEntry&)>& visitor) const -> bool;
  /// Scoped const read of every entry in path order.
  void               ForEachEntry(const std::function<void(const LutLibraryEntry&)>& visitor) const;
  /// Scoped const read of every entry in an installed package's active content.
  void               ForEachPackageEntry(std::string_view                                   package_id,
                                         const std::function<void(const LutLibraryEntry&)>& visitor) const;
  [[nodiscard]] auto EntryCount() const -> std::size_t { return inventory_.entries.size(); }
  [[nodiscard]] auto Diagnostics() const -> const std::vector<LutScanDiagnostic>& {
    return inventory_.diagnostics;
  }

  /// Root-relative path of @p absolute_path when it lies inside the root.
  [[nodiscard]] auto RelativePathInRoot(const std::filesystem::path& absolute_path) const
      -> std::optional<std::string>;

  /// Resolve a listed entry to its file and check that the file exists.
  /// An absent file, or a path that the inventory does not list, requests one
  /// inventory refresh per path; later lookups of the same unresolved path do
  /// not request again until a user-requested refresh.
  auto               LocateEntry(std::string_view relative_path) -> Location;

  [[nodiscard]] auto FavoritePaths() const -> const std::vector<std::string>& {
    return user_state_.favorite_paths;
  }
  [[nodiscard]] auto IsFavorite(std::string_view relative_path) const -> bool;
  /// Add or remove a favorite and persist it. Rejected (kBusy) while a root
  /// operation runs, because that operation carries the favorites to a new root.
  auto               SetFavorite(std::string_view relative_path, bool favorite) -> Status;
  /// Previous roots this library was migrated from (absolute UTF-8 paths).
  [[nodiscard]] auto PreviousRoots() const -> const std::vector<std::string>& {
    return user_state_.previous_roots;
  }

  /// User-requested rescan. Coalesced into a running refresh; kBusy during
  /// another operation. Publishes only after `lut-inventory.json` is written.
  auto             RefreshInventory() -> Status;
  /// Copy selected `.cube` files into `<root>/user/`. Every conflict is checked
  /// before any file is copied; existing files are never overwritten.
  auto             ImportFiles(std::vector<std::filesystem::path> sources) -> Status;
  /// Switch to an existing library root. The root is indexed and its inventory
  /// persisted before the preference changes; failures keep the current root.
  auto             UseRoot(const std::filesystem::path& root) -> Status;
  /// Copy the library to an absent or empty @p destination, verify it, switch
  /// the root, then delete only verified, unchanged source files.
  auto             MigrateRoot(const std::filesystem::path& destination) -> Status;
  /// Open the root in the platform file manager. Returns false and sets
  /// lastError when the operating system rejects the request.
  auto             OpenRootDirectory() -> bool;

  Q_INVOKABLE bool refresh() { return RefreshInventory() == Status::kOk; }
  Q_INVOKABLE bool openRootDirectory() { return OpenRootDirectory(); }
  Q_INVOKABLE bool importFiles(const QStringList& paths);
  Q_INVOKABLE bool useRoot(const QString& path);
  Q_INVOKABLE bool migrateRoot(const QString& path);

 signals:
  /// A new inventory is published; @p affected_paths lists changed entries.
  void InventoryChanged(const QStringList& affected_paths);
  void RootChanged();
  void FavoritesChanged();
  void OperationStateChanged();
  /// An operation completed; details are in LastResult().
  void OperationFinished(alcedo::LutLibraryService::Operation operation,
                         alcedo::LutLibraryService::Status    status);

 private:
  using Completion = std::function<void()>;

  /// Start @p operation on the worker. @p work runs on the worker thread and
  /// returns the owner-thread completion that publishes its result.
  auto Begin(Operation operation, std::function<Completion(std::stop_token)> work) -> Status;
  void Finish(OperationResult result);
  /// Replace the published inventory and announce the changed entry paths.
  auto PublishInventory(LutLibraryInventory inventory) -> std::vector<std::string>;
  void ResumeSourceCleanup();
  void ConvertLegacyFavorites();
  auto RequestRefresh(bool user_requested) -> Status;

  LutLibraryServiceOptions           options_;
  std::filesystem::path              root_;
  LutLibraryInventory                inventory_;
  LutLibraryUserState                user_state_;
  Operation                          operation_ = Operation::kNone;
  OperationResult                    last_result_;
  QString                            last_error_;
  /// Paths for which a lookup already requested a refresh (plan 4.3).
  std::set<std::string, std::less<>> refresh_requested_paths_;
  bool                               shut_down_ = false;
  std::jthread                       worker_;
};

}  // namespace alcedo
