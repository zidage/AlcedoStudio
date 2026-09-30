//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/lut_library_service.hpp"

#include <QDesktopServices>
#include <QDir>
#include <QMetaObject>
#include <QSettings>
#include <algorithm>
#include <system_error>
#include <utility>

#include "utils/lut/lut_inventory_digest.hpp"

namespace alcedo {
namespace {

namespace fs                              = std::filesystem;

constexpr const char* kRootSettingsKey    = "lut/libraryRoot";
constexpr const char* kLegacyFavoritesKey = "editor/lutPanel/favoritePaths";

auto                  ToQString(const fs::path& path) -> QString {
  const std::u8string text = path.u8string();
  return QString::fromUtf8(reinterpret_cast<const char*>(text.data()),
                                            static_cast<qsizetype>(text.size()));
}

auto FromQString(const QString& text) -> fs::path {
  const QByteArray bytes = text.toUtf8();
  return LutPathFromUtf8(
      std::string_view(bytes.constData(), static_cast<std::size_t>(bytes.size())));
}

auto AsciiLower(std::string text) -> std::string {
  for (char& value : text) {
    if (value >= 'A' && value <= 'Z') value = static_cast<char>(value - 'A' + 'a');
  }
  return text;
}

auto HasCubeExtension(const fs::path& path) -> bool {
  return AsciiLower(LutPathToUtf8(path.extension())) == ".cube";
}

auto ToQStringList(const std::vector<std::string>& values) -> QStringList {
  QStringList list;
  list.reserve(static_cast<qsizetype>(values.size()));
  for (const std::string& value : values) list.push_back(QString::fromStdString(value));
  return list;
}

auto ReadReceipts(const fs::path& root) -> std::vector<LutPackageReceipt> {
  std::vector<LutPackageReceipt> receipts;
  std::vector<LutScanDiagnostic> diagnostics;
  ReadLutPackageReceipts(root, &receipts, &diagnostics);
  return receipts;
}

/// Inventory and receipts of an existing root, without network access.
///
/// Content retired by an interrupted installation is removed first. The persisted
/// inventory is used when it agrees with the receipts; a missing, damaged, or
/// outdated inventory (for example after a receipt commit that was not followed
/// by an inventory write) is rebuilt from local files and written back.
struct LoadedLibrary {
  LutLibraryInventory            inventory;
  std::vector<LutPackageReceipt> receipts;
  std::string                    write_error;
};

auto LoadLibraryRoot(const fs::path& root, unsigned workers, const LutLibraryFileOperations& io,
                     const LutLibraryPublication& publication) -> LoadedLibrary {
  LoadedLibrary               loaded;
  LutPackageContentRetirement retired;
  {
    // Inactive content is not published, but the same root may be the active one.
    const auto removal = publication.LockContentForRemoval();
    retired            = RetireInactiveLutPackageContent(root);
  }
  loaded.receipts                           = ReadReceipts(root);
  LutLibraryInventoryParseResult persisted  = ReadLutLibraryInventoryFile(root);
  if (persisted && retired.relocated_user_paths.empty() &&
      LutInventoryMatchesPackageReceipts(*persisted.inventory, loaded.receipts)) {
    loaded.inventory = std::move(*persisted.inventory);
    return loaded;
  }
  loaded.inventory   = ScanLutLibraryRoot(root, workers);
  loaded.write_error = io.write_inventory(root, loaded.inventory);
  return loaded;
}

auto NormalizedRoot(const fs::path& path) -> fs::path {
  std::error_code error;
  fs::path        normalized = fs::weakly_canonical(fs::absolute(path, error), error);
  if (error) normalized = path;
  return normalized.lexically_normal();
}

}  // namespace

// ── Preferences and helpers ─────────────────────────────────────────────────

auto SettingsLutLibraryPreferences::LoadRoot() const -> std::optional<fs::path> {
  const QString value = QSettings().value(QString::fromLatin1(kRootSettingsKey)).toString();
  if (value.isEmpty()) return std::nullopt;
  return FromQString(value);
}

auto SettingsLutLibraryPreferences::SaveRoot(const fs::path& root) -> bool {
  QSettings settings;
  settings.setValue(QString::fromLatin1(kRootSettingsKey), ToQString(root));
  settings.sync();
  return settings.status() == QSettings::NoError;
}

auto SettingsLutLibraryPreferences::LoadLegacyFavoritePaths() const -> QStringList {
  return QSettings().value(QString::fromLatin1(kLegacyFavoritesKey)).toStringList();
}

void SettingsLutLibraryPreferences::SaveLegacyFavoritePaths(const QStringList& paths) {
  QSettings settings;
  if (paths.isEmpty()) {
    settings.remove(QString::fromLatin1(kLegacyFavoritesKey));
  } else {
    settings.setValue(QString::fromLatin1(kLegacyFavoritesKey), paths);
  }
}

auto DefaultLutLibraryRoot() -> fs::path {
  return FromQString(QDir::homePath()) / ".alcedo" / "luts";
}

auto LutLibraryDirectoryUrl(const fs::path& directory) -> QUrl {
  return QUrl::fromLocalFile(ToQString(directory));
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

LutLibraryService::LutLibraryService(LutLibraryServiceOptions options, QObject* parent)
    : QObject(parent), options_(std::move(options)) {
  if (!options_.preferences)
    options_.preferences = std::make_unique<SettingsLutLibraryPreferences>();
  if (!options_.open_url) {
    options_.open_url = [](const QUrl& url) { return QDesktopServices::openUrl(url); };
  }
  publication_ = std::make_shared<LutLibraryPublication>(
      NormalizedRoot(options_.preferences->LoadRoot().value_or(options_.default_root)));
  // Called on render threads; the queued call runs on this object's thread. Clearing the
  // observer in Shutdown waits for running calls, and destroying this object drops posted calls.
  publication_->SetMissingObserver([this](const LutReference& reference) {
    QMetaObject::invokeMethod(
        this, [this, key = DescribeLutReference(reference)] { RequestRefreshForMissing(key); },
        Qt::QueuedConnection);
  });
}

LutLibraryService::~LutLibraryService() { Shutdown(); }

void LutLibraryService::Shutdown() {
  shut_down_ = true;
  publication_->SetMissingObserver({});
  if (worker_.joinable()) {
    worker_.request_stop();
    worker_.join();
  }
}

void LutLibraryService::Start() {
  std::error_code error;
  if (!options_.preferences->LoadRoot() && !fs::exists(Root(), error)) {
    fs::create_directories(Root(), error);
  }
  const fs::path root    = Root();
  const unsigned workers = options_.scan_worker_count;
  Begin(Operation::kLoad, [this, root, workers](std::stop_token) -> Completion {
    OperationResult result{.operation = Operation::kLoad};
    std::error_code error;
    if (!fs::is_directory(root, error)) {
      result.status  = Status::kRootUnavailable;
      result.message = "The LUT library folder is unavailable: " + LutPathToUtf8(root);
      return [this, result] { Finish(result); };
    }
    // A missing, damaged, or outdated inventory is rebuilt from local files (plan 4.3).
    auto loaded = std::make_shared<LoadedLibrary>(
        LoadLibraryRoot(root, workers, options_.file_operations, *publication_));
    if (!loaded->write_error.empty()) {
      result.status  = Status::kPersistenceError;
      result.message = "The rebuilt LUT inventory cannot be saved: " + loaded->write_error;
    }
    LutLibraryUserStateReadResult state = ReadLutLibraryUserStateFile(root);
    if (!state.state && result.status == Status::kOk) {
      result.status  = Status::kPersistenceError;
      result.message = state.error;
    }
    auto user_state =
        std::make_shared<LutLibraryUserState>(state.state.value_or(LutLibraryUserState{}));
    return [this, result, loaded, user_state]() mutable {
      publication_->SetUserState(std::move(*user_state));
      result.affected_paths =
          PublishInventory(std::move(loaded->inventory), std::move(loaded->receipts));
      emit FavoritesChanged();
      Finish(std::move(result));
      ConvertLegacyFavorites();
      ResumeSourceCleanup();
    };
  });
}

auto LutLibraryService::Begin(Operation operation, std::function<Completion(std::stop_token)> work)
    -> Status {
  if (shut_down_) return Status::kCanceled;
  if (operation_ != Operation::kNone) return Status::kBusy;
  operation_ = operation;
  last_error_.clear();
  emit OperationStateChanged();
  // Move-assignment joins the previous worker, which has already posted its completion.
  worker_ = std::jthread([this, work = std::move(work)](std::stop_token stop) {
    Completion completion = work(stop);
    QMetaObject::invokeMethod(
        this,
        [this, completion = std::move(completion)] {
          if (!shut_down_) completion();
        },
        Qt::QueuedConnection);
  });
  return Status::kOk;
}

void LutLibraryService::Finish(OperationResult result) {
  operation_   = Operation::kNone;
  last_result_ = std::move(result);
  last_error_ =
      last_result_.status == Status::kOk ? QString() : QString::fromStdString(last_result_.message);
  emit OperationStateChanged();
  emit OperationFinished(last_result_.operation, last_result_.status);
  // A handler may have started another operation; the retirement then follows that one.
  if (pending_content_retirement_ && operation_ == Operation::kNone && !shut_down_) {
    pending_content_retirement_ = false;
    RetireReplacedPackageContent();
  }
}

auto LutLibraryService::PublishInventory(LutLibraryInventory                           inventory,
                                         std::optional<std::vector<LutPackageReceipt>> receipts)
    -> std::vector<std::string> {
  std::vector<std::string> affected =
      ChangedLutLibraryEntryPaths(publication_->Inventory(), inventory);
  publication_->PublishInventory(std::move(inventory), std::move(receipts));
  emit InventoryChanged(ToQStringList(affected));
  return affected;
}

void LutLibraryService::ResumeSourceCleanup() {
  std::error_code error;
  if (!fs::exists(Root() / LutPathFromUtf8(kLutMigrationCleanupFileName), error)) return;
  const fs::path root = Root();
  Begin(Operation::kSourceCleanup, [this, root](std::stop_token stop) -> Completion {
    // A render may still read a source file it resolved before the root switch; each
    // deletion waits for such reads (plan 4.6). Hash checks run without the lock.
    LutLibraryFileOperations io = options_.file_operations;
    io.remove_file = [publication = publication_, remove = io.remove_file](const fs::path& path) {
      const auto removal = publication->LockContentForRemoval();
      return remove(path);
    };
    auto cleanup = std::make_shared<LutLibraryMigrationCleanup>(
        CleanLutLibraryMigrationSource(root, io, stop));
    return [this, cleanup] {
      OperationResult result{.operation = Operation::kSourceCleanup};
      result.kept_source_paths = cleanup->kept_changed_paths;
      if (!cleanup->error.empty()) {
        result.status  = Status::kIoError;
        result.message = cleanup->error;
      } else if (!cleanup->kept_changed_paths.empty()) {
        result.message = "Source files that changed after copying were kept.";
      }
      Finish(std::move(result));
    };
  });
}

void LutLibraryService::ConvertLegacyFavorites() {
  const QStringList   legacy   = options_.preferences->LoadLegacyFavoritePaths();
  LutLibraryUserState updated  = publication_->UserState();
  std::vector<std::string> paths = std::move(updated.legacy_favorite_paths);
  updated.legacy_favorite_paths.clear();
  QStringList remaining;
  for (const QString& path : legacy) {
    if (std::optional<std::string> relative = RelativePathInRoot(FromQString(path))) {
      paths.push_back(std::move(*relative));
    } else {
      remaining.push_back(path);
    }
  }
  if (paths.empty()) return;
  for (const std::string& relative : paths) {
    const LutLibraryEntry* entry = FindLutLibraryEntry(publication_->Inventory(), relative);
    std::string id = entry != nullptr ? LutLibraryPublication::EntryIdOf(*entry)
                                      : DescribeLutReference(LibraryLutReference{relative});
    auto position  = std::lower_bound(updated.favorite_entry_ids.begin(),
                                      updated.favorite_entry_ids.end(), id);
    if (position == updated.favorite_entry_ids.end() || *position != id) {
      updated.favorite_entry_ids.insert(position, std::move(id));
    }
  }
  if (!options_.file_operations.write_user_state(Root(), updated).empty()) return;
  publication_->SetUserState(std::move(updated));
  if (remaining.size() != legacy.size()) options_.preferences->SaveLegacyFavoritePaths(remaining);
  emit FavoritesChanged();
}

// ── Reads ───────────────────────────────────────────────────────────────────

auto LutLibraryService::root_path() const -> QString { return ToQString(Root()); }

auto LutLibraryService::ReadEntry(std::string_view                                   relative_path,
                                  const std::function<void(const LutLibraryEntry&)>& visitor) const
    -> bool {
  const LutLibraryEntry* entry = FindLutLibraryEntry(publication_->Inventory(), relative_path);
  if (entry == nullptr) return false;
  visitor(*entry);
  return true;
}

void LutLibraryService::ForEachEntry(
    const std::function<void(const LutLibraryEntry&)>& visitor) const {
  for (const LutLibraryEntry& entry : publication_->Inventory().entries) visitor(entry);
}

void LutLibraryService::ForEachPackageEntry(
    std::string_view package_id, const std::function<void(const LutLibraryEntry&)>& visitor) const {
  if (package_id.empty()) return;
  for (const LutLibraryEntry& entry : publication_->Inventory().entries) {
    if (entry.managed_package_id == package_id) visitor(entry);
  }
}

auto LutLibraryService::RelativePathInRoot(const fs::path& absolute_path) const
    -> std::optional<std::string> {
  const fs::path relative = absolute_path.lexically_normal().lexically_relative(Root());
  if (relative.empty() || *relative.begin() == "..") return std::nullopt;
  std::string utf8 = LutPathToUtf8(relative);
  if (!IsSafeLutRelativePath(utf8)) return std::nullopt;
  return utf8;
}

auto LutLibraryService::LocateEntry(std::string_view relative_path) -> Location {
  Location location;
  if (!IsSafeLutRelativePath(relative_path)) return location;
  location.absolute_path = Root() / LutPathFromUtf8(relative_path);
  std::error_code error;
  if (FindLutLibraryEntry(publication_->Inventory(), relative_path) != nullptr) {
    if (fs::is_regular_file(location.absolute_path, error)) {
      location.status = LocateStatus::kFound;
      return location;
    }
    location.status = LocateStatus::kMissing;
  }
  // One refresh per unresolved path; a user-requested refresh clears this set.
  if (refresh_requested_paths_.emplace(relative_path).second &&
      RequestRefresh(false) != Status::kOk) {
    refresh_requested_paths_.erase(refresh_requested_paths_.find(relative_path));
  }
  return location;
}

auto LutLibraryService::ReadEntryById(
    std::string_view entry_id, const std::function<void(const LutLibraryEntry&)>& visitor) const
    -> bool {
  constexpr std::string_view kLibraryPrefix = "library:";
  if (entry_id.starts_with(kLibraryPrefix)) {
    const LutLibraryEntry* entry =
        FindLutLibraryEntry(publication_->Inventory(), entry_id.substr(kLibraryPrefix.size()));
    // A path of package-owned official content is selected by its official ID instead.
    if (entry == nullptr || LutLibraryPublication::EntryIdOf(*entry) != entry_id) return false;
    visitor(*entry);
    return true;
  }
  for (const LutLibraryEntry& entry : publication_->Inventory().entries) {
    if (!entry.managed_package_id.empty() && LutLibraryPublication::EntryIdOf(entry) == entry_id) {
      visitor(entry);
      return true;
    }
  }
  return false;
}

auto LutLibraryService::IsFavorite(std::string_view entry_id) const -> bool {
  const auto& favorites = publication_->UserState().favorite_entry_ids;
  return std::binary_search(favorites.begin(), favorites.end(), entry_id);
}

auto LutLibraryService::SetFavorite(std::string_view entry_id, bool favorite) -> Status {
  if (operation_ == Operation::kLoad || operation_ == Operation::kUseRoot ||
      operation_ == Operation::kMigrateRoot) {
    return Status::kBusy;
  }
  if (!IsValidLutLibraryEntryId(entry_id)) return Status::kInvalidRequest;
  if (IsFavorite(entry_id) == favorite) return Status::kOk;
  LutLibraryUserState updated  = publication_->UserState();
  auto&               ids      = updated.favorite_entry_ids;
  auto                position = std::lower_bound(ids.begin(), ids.end(), entry_id);
  if (favorite) {
    ids.insert(position, std::string(entry_id));
  } else {
    ids.erase(position);
  }
  if (std::string error = options_.file_operations.write_user_state(Root(), updated);
      !error.empty()) {
    last_error_ = QString::fromStdString("The LUT favorites cannot be saved: " + error);
    emit OperationStateChanged();
    return Status::kPersistenceError;
  }
  publication_->SetUserState(std::move(updated));
  emit FavoritesChanged();
  return Status::kOk;
}

// ── Operations ──────────────────────────────────────────────────────────────

auto LutLibraryService::RefreshInventory() -> Status { return RequestRefresh(true); }

auto LutLibraryService::RequestRefresh(bool user_requested) -> Status {
  if (user_requested) refresh_requested_paths_.clear();
  if (operation_ == Operation::kRefresh) return Status::kOk;  // Coalesced into the running scan.
  const fs::path root    = Root();
  const unsigned workers = options_.scan_worker_count;
  return Begin(Operation::kRefresh, [this, root, workers](std::stop_token) -> Completion {
    auto        scanned  = std::make_shared<LutLibraryInventory>(ScanLutLibraryRoot(root, workers));
    auto        receipts = std::make_shared<std::vector<LutPackageReceipt>>(ReadReceipts(root));
    std::string error    = options_.file_operations.write_inventory(root, *scanned);
    return [this, scanned, receipts, error] {
      OperationResult result{.operation = Operation::kRefresh};
      if (!error.empty()) {
        // Keep the previous inventory; its verification is now out of date.
        result.status  = Status::kPersistenceError;
        result.message = "The LUT inventory cannot be saved: " + error;
      } else {
        result.affected_paths = PublishInventory(std::move(*scanned), std::move(*receipts));
        if (!inventory_complete()) result.message = "Some LUT folders or files could not be read.";
      }
      Finish(std::move(result));
    };
  });
}

auto LutLibraryService::ImportFiles(std::vector<fs::path> sources) -> Status {
  if (sources.empty()) return Status::kInvalidRequest;
  const fs::path root = Root();
  return Begin(Operation::kImport, [this, root, sources](std::stop_token) -> Completion {
    OperationResult          result{.operation = Operation::kImport};
    std::vector<std::string> targets;
    std::vector<std::string> problems;
    std::error_code          error;
    for (const fs::path& source : sources) {
      const std::string name   = LutPathToUtf8(source.filename());
      const std::string target = std::string(kLutUserImportDirectoryName) + "/" + name;
      if (!fs::is_regular_file(source, error) || !HasCubeExtension(source)) {
        problems.push_back(LutPathToUtf8(source) + " is not a .cube file.");
      } else if (!IsSafeLutRelativePath(target)) {
        problems.push_back(name + " is not a valid file name.");
      } else if (std::any_of(targets.begin(), targets.end(), [&](const std::string& other) {
                   return AsciiLower(other) == AsciiLower(target);
                 })) {
        problems.push_back(name + " is selected more than once.");
      } else if (fs::exists(root / LutPathFromUtf8(target), error)) {
        problems.push_back(target + " already exists in the LUT library.");
      }
      targets.push_back(target);
    }
    if (!problems.empty()) {
      result.status  = Status::kConflict;
      result.message = "No files were imported. ";
      for (const std::string& problem : problems) result.message += problem + " ";
      return [this, result] { Finish(result); };
    }

    const auto rollback = [&](std::size_t copied) {
      for (std::size_t index = 0; index < copied; ++index) {
        std::error_code ignored;
        fs::remove(root / LutPathFromUtf8(targets[index]), ignored);
      }
    };
    fs::create_directories(root / LutPathFromUtf8(kLutUserImportDirectoryName), error);
    for (std::size_t index = 0; index < sources.size(); ++index) {
      std::string copy_error = options_.file_operations.copy_file(
          sources[index], root / LutPathFromUtf8(targets[index]));
      if (!copy_error.empty()) {
        rollback(index);
        result.status  = Status::kIoError;
        result.message = "Cannot import " + targets[index] + ": " + copy_error;
        return [this, result] { Finish(result); };
      }
    }
    // The replacement inventory exists only to be persisted and then published;
    // it is moved into the owner on completion (plan 5.1). The published inventory does not
    // change while this operation runs.
    auto merged = std::make_shared<LutLibraryInventory>(publication_->Inventory());
    for (const std::string& target : targets) {
      LutLibraryEntry entry = ClassifyLutLibraryFile(root, target);
      auto position = std::lower_bound(merged->entries.begin(), merged->entries.end(), target,
                                       [](const LutLibraryEntry& item, const std::string& path) {
                                         return item.relative_path < path;
                                       });
      if (position != merged->entries.end() && position->relative_path == target) {
        *position = std::move(entry);
      } else {
        merged->entries.insert(position, std::move(entry));
      }
    }
    if (std::string write_error = options_.file_operations.write_inventory(root, *merged);
        !write_error.empty()) {
      rollback(targets.size());
      result.status  = Status::kPersistenceError;
      result.message = "The LUT inventory cannot be saved: " + write_error;
      return [this, result] { Finish(result); };
    }
    return [this, result, merged]() mutable {
      result.affected_paths = PublishInventory(std::move(*merged));
      Finish(std::move(result));
    };
  });
}

auto LutLibraryService::UseRoot(const fs::path& root) -> Status {
  if (root.empty()) return Status::kInvalidRequest;
  const fs::path target  = NormalizedRoot(root);
  const unsigned workers = options_.scan_worker_count;
  return Begin(Operation::kUseRoot, [this, target, workers](std::stop_token) -> Completion {
    OperationResult result{.operation = Operation::kUseRoot};
    std::error_code error;
    const auto      fail = [&](Status status, std::string message) -> Completion {
      result.status  = status;
      result.message = std::move(message);
      return [this, result] { Finish(result); };
    };
    if (!fs::is_directory(target, error)) {
      return fail(Status::kRootUnavailable, "The folder does not exist: " + LutPathToUtf8(target));
    }
    auto loaded = std::make_shared<LoadedLibrary>(
        LoadLibraryRoot(target, workers, options_.file_operations, *publication_));
    if (!loaded->write_error.empty()) {
      return fail(Status::kPersistenceError,
                  "The LUT inventory cannot be saved in the folder: " + loaded->write_error);
    }
    LutLibraryUserStateReadResult state = ReadLutLibraryUserStateFile(target);
    if (!state.state) return fail(Status::kPersistenceError, state.error);
    auto user_state = std::make_shared<LutLibraryUserState>(std::move(*state.state));
    return [this, result, target, loaded, user_state]() mutable {
      if (!options_.preferences->SaveRoot(target)) {
        result.status  = Status::kPreferenceError;
        result.message = "The LUT library folder choice cannot be saved.";
        Finish(std::move(result));
        return;
      }
      publication_->SetRoot(target);
      publication_->SetUserState(std::move(*user_state));
      refresh_requested_paths_.clear();
      result.affected_paths =
          PublishInventory(std::move(loaded->inventory), std::move(loaded->receipts));
      emit RootChanged();
      emit FavoritesChanged();
      Finish(std::move(result));
      ConvertLegacyFavorites();
      ResumeSourceCleanup();
    };
  });
}

auto LutLibraryService::MigrateRoot(const fs::path& destination) -> Status {
  if (destination.empty()) return Status::kInvalidRequest;
  const fs::path source = Root();
  const fs::path target = NormalizedRoot(destination);
  return Begin(Operation::kMigrateRoot, [this, source, target](std::stop_token stop) -> Completion {
    auto preparation = std::make_shared<LutLibraryMigrationPreparation>(
        PrepareLutLibraryMigration(source, target, publication_->Inventory(),
                                   publication_->UserState(), options_.file_operations, stop));
    return [this, target, preparation] {
      OperationResult result{.operation = Operation::kMigrateRoot};
      if (!preparation->error.empty()) {
        result.status  = preparation->canceled ? Status::kCanceled : Status::kIoError;
        result.message = preparation->error;
        Finish(std::move(result));
        return;
      }
      // Persisting the root choice is the commit point. Before it, the source
      // stays the active library and the prepared destination is removed.
      if (!options_.preferences->SaveRoot(target)) {
        DiscardPreparedLutLibraryMigration(target);
        result.status  = Status::kPreferenceError;
        result.message = "The new LUT library folder choice cannot be saved.";
        Finish(std::move(result));
        return;
      }
      publication_->SetRoot(target);
      publication_->SetUserState(std::move(preparation->user_state));
      refresh_requested_paths_.clear();
      // Entries keep their relative paths; announce the new root so render consumers resolve
      // library references through it.
      emit InventoryChanged({});
      emit RootChanged();
      emit FavoritesChanged();
      Finish(std::move(result));
      ResumeSourceCleanup();
    };
  });
}

auto LutLibraryService::InstallPackage(LutPackageInstallRequest request) -> Status {
  if (!IsLutPackageId(request.expected.package_id) || request.archive_path.empty()) {
    return Status::kInvalidRequest;
  }
  const fs::path root    = Root();
  const unsigned workers = options_.scan_worker_count;
  return Begin(
      Operation::kInstallPackage,
      [this, root, workers, request = std::move(request)](std::stop_token stop) -> Completion {
        OperationResult result{.operation = Operation::kInstallPackage};
        const QString   package_id = QString::fromStdString(request.expected.package_id);
        // The stage is announced on the owner thread; this object outlives the worker.
        const auto      on_stage   = [this, package_id](LutPackageInstallStage stage) {
          QMetaObject::invokeMethod(
              this,
              [this, package_id, stage] {
                if (!shut_down_) emit PackageInstallStageChanged(package_id, stage);
              },
              Qt::QueuedConnection);
        };
        LutPackageInstallSteps steps;
        steps.write_receipt            = options_.file_operations.write_package_receipt;
        steps.lock_content_for_removal = [publication = publication_] {
          return publication->LockContentForRemoval();
        };
        const LutPackageInstallOutcome outcome =
            InstallLutPackageArchive(root, request, steps, stop, on_stage);
        if (!outcome.committed) {
          result.status  = outcome.canceled ? Status::kCanceled : Status::kIoError;
          result.message = "The LUT package was not installed: " + outcome.error;
          return [this, result] { Finish(result); };
        }
        result.committed_package_id = request.expected.package_id;
        // The receipt is committed; the rescan lists the new content.
        auto scanned  = std::make_shared<LutLibraryInventory>(ScanLutLibraryRoot(root, workers));
        auto receipts = std::make_shared<std::vector<LutPackageReceipt>>(ReadReceipts(root));
        std::string error = options_.file_operations.write_inventory(root, *scanned);
        if (!error.empty()) {
          result.status = Status::kPersistenceError;
          result.message =
              "The LUT package is installed; an inventory refresh is required: " + error;
          return [this, result] { Finish(result); };
        }
        return [this, result, scanned, receipts]() mutable {
          result.affected_paths = PublishInventory(std::move(*scanned), std::move(*receipts));
          // Renders now resolve to the new content; the replaced content is retired next.
          pending_content_retirement_ = true;
          Finish(std::move(result));
        };
      });
}

void LutLibraryService::RetireReplacedPackageContent() {
  const fs::path root    = Root();
  const unsigned workers = options_.scan_worker_count;
  Begin(Operation::kRetirePackageContent, [this, root, workers](std::stop_token) -> Completion {
    OperationResult             result{.operation = Operation::kRetirePackageContent};
    LutPackageContentRetirement retired;
    {
      // Renders that resolved the replaced content before the publication finish reading it.
      const auto removal = publication_->LockContentForRemoval();
      retired            = RetireInactiveLutPackageContent(root);
    }
    if (!retired.problems.empty()) {
      result.status  = Status::kIoError;
      result.message = "The previous package content could not be removed completely: " +
                       retired.problems.front();
    }
    if (retired.relocated_user_paths.empty()) {
      return [this, result] { Finish(result); };
    }
    // Relocated user-declared files become user entries of the library.
    auto scanned  = std::make_shared<LutLibraryInventory>(ScanLutLibraryRoot(root, workers));
    auto receipts = std::make_shared<std::vector<LutPackageReceipt>>(ReadReceipts(root));
    if (std::string error = options_.file_operations.write_inventory(root, *scanned);
        !error.empty()) {
      result.status  = Status::kPersistenceError;
      result.message = "The LUT inventory cannot be saved: " + error;
      return [this, result] { Finish(result); };
    }
    return [this, result, scanned, receipts]() mutable {
      result.affected_paths = PublishInventory(std::move(*scanned), std::move(*receipts));
      Finish(std::move(result));
    };
  });
}

void LutLibraryService::RequestRefreshForMissing(const std::string& reference_key) {
  if (shut_down_) return;
  // One refresh per unresolved reference; a user-requested refresh clears this set (plan 4.3).
  if (refresh_requested_paths_.emplace(reference_key).second &&
      RequestRefresh(false) != Status::kOk) {
    refresh_requested_paths_.erase(refresh_requested_paths_.find(reference_key));
  }
}

auto LutLibraryService::ReferenceForPath(const fs::path& absolute_path) const
    -> std::optional<LutReference> {
  const std::optional<std::string> relative = RelativePathInRoot(absolute_path);
  if (!relative) return std::nullopt;
  const LutLibraryEntry* entry = FindLutLibraryEntry(publication_->Inventory(), *relative);
  if (entry == nullptr) return std::nullopt;
  return LutLibraryPublication::ReferenceForEntry(*entry);
}

auto LutLibraryService::CancelOperation() -> bool {
  if (operation_ != Operation::kInstallPackage || !worker_.joinable()) return false;
  worker_.request_stop();
  return true;
}

auto LutLibraryService::OpenRootDirectory() -> bool {
  std::error_code error;
  const bool      opened =
      fs::is_directory(Root(), error) && options_.open_url(LutLibraryDirectoryUrl(Root()));
  if (!opened) {
    last_error_ = tr("The LUT folder could not be opened: %1").arg(root_path());
    emit OperationStateChanged();
  }
  return opened;
}

auto LutLibraryService::importFiles(const QStringList& paths) -> bool {
  std::vector<fs::path> sources;
  for (const QString& path : paths) {
    const QUrl url(path);
    sources.push_back(FromQString(url.isLocalFile() ? url.toLocalFile() : path));
  }
  return ImportFiles(std::move(sources)) == Status::kOk;
}

auto LutLibraryService::useRoot(const QString& path) -> bool {
  return UseRoot(FromQString(path)) == Status::kOk;
}

auto LutLibraryService::migrateRoot(const QString& path) -> bool {
  return MigrateRoot(FromQString(path)) == Status::kOk;
}

}  // namespace alcedo
