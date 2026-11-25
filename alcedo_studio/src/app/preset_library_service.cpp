//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/preset_library_service.hpp"

#include <QDir>
#include <QFile>
#include <QMetaObject>
#include <QSettings>
#include <QUrl>
#include <algorithm>
#include <exception>
#include <system_error>

#include "app/document_transfer.hpp"
#include "edit/operators/models/builtin_type_ids.hpp"
#include "edit/operators/models/lmt_model.hpp"

namespace alcedo {
namespace {

namespace fs                           = std::filesystem;

constexpr const char* kRootSettingsKey = "presets/libraryRoot";
/// Most numbered names that a save or an add tries before it reports a conflict.
constexpr int         kMaxNameNumber   = 999;

auto                  Utf8(const fs::path& path) -> std::string {
  const auto text = path.u8string();
  return {reinterpret_cast<const char*>(text.data()), text.size()};
}

auto PathFromUtf8(std::string_view text) -> fs::path {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

auto ToQString(const fs::path& path) -> QString { return QString::fromStdString(Utf8(path)); }

auto FromQString(const QString& text) -> fs::path { return PathFromUtf8(text.toStdString()); }

auto NormalizedRoot(const fs::path& path) -> fs::path {
  std::error_code error;
  fs::path        normalized = fs::weakly_canonical(fs::absolute(path, error), error);
  if (error) {
    normalized = path;
  }
  return normalized.lexically_normal();
}

auto DefaultMoveToTrash(const fs::path& path, std::string* error) -> bool {
  QFile file(ToQString(path));
  if (file.moveToTrash()) {
    return true;
  }
  *error = file.errorString().toStdString();
  return false;
}

/// Status of a failed preset file read or write.
auto StatusForRule(PresetFileRule rule) -> PresetLibraryService::Status {
  switch (rule) {
    case PresetFileRule::kNone:
      return PresetLibraryService::Status::kOk;
    case PresetFileRule::kTargetExists:
      return PresetLibraryService::Status::kConflict;
    case PresetFileRule::kInvalidName:
      return PresetLibraryService::Status::kInvalidRequest;
    case PresetFileRule::kReadFailed:
    case PresetFileRule::kWriteFailed:
      return PresetLibraryService::Status::kIoError;
    default:
      return PresetLibraryService::Status::kRefused;
  }
}

/**
 * Replace each LMT `cube_path` of @p adjustments with the reference of its LUT library entry
 * (plan 3.3). Returns the reason when a file is not in the LUT library.
 */
auto ReplaceLutFilePaths(
    std::vector<TransferAdjustmentValue>* adjustments, std::string_view node,
    const std::function<std::optional<LutReference>(const fs::path&)>& reference_for_path)
    -> std::string {
  for (auto& adjustment : *adjustments) {
    if (adjustment.type != type_ids::Lmt()) {
      continue;
    }
    const LmtUpdate update = LmtUpdateFromModelJson(adjustment.params);
    const auto*     file =
        update.reference.has_value() ? std::get_if<FileLutReference>(&*update.reference) : nullptr;
    if (file == nullptr) {
      continue;
    }
    const std::optional<LutReference> reference = reference_for_path
                                                      ? reference_for_path(PathFromUtf8(file->path))
                                                      : std::optional<LutReference>{};
    if (!reference.has_value() || std::holds_alternative<FileLutReference>(*reference) ||
        IsEmptyLutReference(*reference)) {
      return "The LUT " + Utf8(PathFromUtf8(file->path).filename()) + " of " + std::string{node} +
             " is not in the LUT library. Add the LUT to the LUT library, or save the preset "
             "without this item.";
    }
    adjustment.params["cube_path"] = std::string{};
    adjustment.params["reference"] = TaggedLutReferenceToJson(*reference);
  }
  return {};
}

}  // namespace

// ── Preferences ─────────────────────────────────────────────────────────────

auto SettingsPresetLibraryPreferences::LoadRoot() const -> std::optional<fs::path> {
  const QString value = QSettings().value(QString::fromLatin1(kRootSettingsKey)).toString();
  if (value.isEmpty()) {
    return std::nullopt;
  }
  return FromQString(value);
}

auto SettingsPresetLibraryPreferences::SaveRoot(const fs::path& root) -> bool {
  QSettings settings;
  settings.setValue(QString::fromLatin1(kRootSettingsKey), ToQString(root));
  settings.sync();
  return settings.status() == QSettings::NoError;
}

auto DefaultPresetLibraryRoot() -> fs::path {
  return FromQString(QDir::homePath()) / ".alcedo" / "presets";
}

// ── Lifecycle ───────────────────────────────────────────────────────────────

PresetLibraryService::PresetLibraryService(PresetLibraryServiceOptions options, QObject* parent)
    : QObject(parent), options_(std::move(options)) {
  if (!options_.preferences) {
    options_.preferences = std::make_unique<SettingsPresetLibraryPreferences>();
  }
  if (!options_.move_to_trash) {
    options_.move_to_trash = DefaultMoveToTrash;
  }
  root_        = NormalizedRoot(options_.preferences->LoadRoot().value_or(options_.default_root));
  worker_root_ = root_;
}

PresetLibraryService::~PresetLibraryService() { Shutdown(); }

void PresetLibraryService::Shutdown() {
  shut_down_ = true;
  worker_.Shutdown();
}

void PresetLibraryService::Start() {
  std::error_code error;
  if (!options_.preferences->LoadRoot() && !fs::exists(root_, error)) {
    fs::create_directories(root_, error);
  }
  Refresh();
}

auto PresetLibraryService::root_path() const -> QString {
  return QDir::toNativeSeparators(ToQString(root_));
}

auto PresetLibraryService::FindEntry(std::string_view id) const -> const PresetEntry* {
  return FindPresetEntry(inventory_, id);
}

// ── Operation queue ─────────────────────────────────────────────────────────

void PresetLibraryService::Submit(Operation operation, std::function<WorkerResult()> work) {
  if (shut_down_) {
    return;
  }
  ++pending_operations_;
  const std::uint64_t request = ++last_request_;
  if (pending_operations_ == 1) {
    emit OperationStateChanged();
  }
  worker_.Submit([this, operation, request, work = std::move(work)] {
    WorkerResult worker_result;
    try {
      worker_result = work();
    } catch (const std::exception& e) {
      worker_result.result.status  = Status::kIoError;
      worker_result.result.message = e.what();
    }
    worker_result.result.operation = operation;
    worker_result.result.request   = request;
    QMetaObject::invokeMethod(
        this,
        [this, worker_result = std::move(worker_result)]() mutable {
          if (!shut_down_) {
            Publish(std::move(worker_result));
          }
        },
        Qt::QueuedConnection);
  });
}

void PresetLibraryService::Publish(WorkerResult worker_result) {
  const bool root_changed = worker_result.root.has_value() && *worker_result.root != root_;
  if (worker_result.root.has_value()) {
    root_ = std::move(*worker_result.root);
  }
  if (root_changed) {
    cache_.clear();
  }
  if (worker_result.inventory.has_value()) {
    inventory_ = std::move(*worker_result.inventory);
    PruneCache();
  }
  --pending_operations_;
  last_result_ = std::move(worker_result.result);
  last_error_ =
      last_result_.status == Status::kOk ? QString() : QString::fromStdString(last_result_.message);
  if (root_changed) {
    emit RootChanged();
  }
  if (worker_result.inventory.has_value()) {
    emit InventoryChanged();
  }
  emit OperationStateChanged();
  emit OperationFinished(last_result_.operation, last_result_.status);
}

auto PresetLibraryService::Refuse(Operation operation, Status status, std::string message)
    -> Status {
  last_result_ = OperationResult{.operation = operation, .status = status, .message = message};
  last_error_  = QString::fromStdString(message);
  emit OperationStateChanged();
  emit OperationFinished(operation, status);
  return status;
}

void PresetLibraryService::ScanWorkerRoot(WorkerResult* worker_result) const {
  worker_result->inventory = ScanPresetLibraryRoot(worker_root_);
}

auto PresetLibraryService::FreePresetName(const fs::path& folder, std::string_view name,
                                          std::string* error) const -> std::optional<std::string> {
  for (int number = 1; number <= kMaxNameNumber; ++number) {
    std::string candidate{name};
    if (number > 1) {
      candidate += " " + std::to_string(number);
    }
    if (auto name_error = PresetNameError(candidate); !name_error.empty()) {
      *error = name_error;
      return std::nullopt;
    }
    std::error_code code;
    if (!fs::exists(fs::symlink_status(folder / PresetFileNameFor(candidate), code))) {
      return candidate;
    }
  }
  *error = "Each numbered form of the name '" + std::string{name} + "' exists.";
  return std::nullopt;
}

void PresetLibraryService::PruneCache() {
  cache_.remove_if([this](const CachedPackage& cached) {
    const PresetEntry* entry = FindEntry(cached.id);
    return entry == nullptr || !entry->available;
  });
}

// ── Package cache ───────────────────────────────────────────────────────────

auto PresetLibraryService::PackageFor(std::string_view id) -> PresetPackageResult {
  const PresetEntry* entry = FindEntry(id);
  if (entry == nullptr) {
    return {.rule    = PresetFileRule::kReadFailed,
            .message = "The preset '" + std::string{id} + "' is not in the preset folder."};
  }
  const fs::path  path = root_ / PathFromUtf8(entry->id);
  std::error_code code;
  const auto      file_size  = fs::file_size(path, code);
  const auto      write_time = fs::last_write_time(path, code);
  if (code) {
    return {.rule    = PresetFileRule::kReadFailed,
            .message = "The preset file " + entry->id + " cannot be read: " + code.message()};
  }
  const auto cached = std::find_if(cache_.begin(), cache_.end(),
                                   [id](const CachedPackage& item) { return item.id == id; });
  if (cached != cache_.end()) {
    if (cached->file_size == file_size && cached->write_time == write_time) {
      cache_.splice(cache_.begin(), cache_, cached);
      return {.package = cache_.front().package};
    }
    cache_.erase(cached);
  }
  ++package_file_reads_;
  PresetFileReadResult read = ReadPresetFile(path);
  if (!read.Ok()) {
    return {.rule = read.rule, .message = std::move(read.message)};
  }
  auto package = std::make_shared<const AdjustmentTransferPackage>(std::move(*read.package));
  cache_.push_front({.id         = std::string{id},
                     .file_size  = file_size,
                     .write_time = write_time,
                     .package    = package});
  if (cache_.size() > kPresetPackageCacheCapacity) {
    cache_.pop_back();
  }
  return {.package = std::move(package)};
}

// ── Operations ──────────────────────────────────────────────────────────────

void PresetLibraryService::Refresh() {
  Submit(Operation::kScan, [this] {
    WorkerResult worker_result;
    ScanWorkerRoot(&worker_result);
    return worker_result;
  });
}

void PresetLibraryService::useRoot(const QString& folder) {
  const QUrl url(folder);
  UseRoot(FromQString(url.isLocalFile() ? url.toLocalFile() : folder));
}

void PresetLibraryService::UseRoot(const fs::path& root) {
  const fs::path normalized = NormalizedRoot(root);
  Submit(Operation::kUseRoot, [this, normalized] {
    WorkerResult    worker_result;
    std::error_code code;
    if (!fs::is_directory(normalized, code)) {
      worker_result.result.status  = Status::kRootUnavailable;
      worker_result.result.message = "The folder " + Utf8(normalized) + " is not available.";
      return worker_result;
    }
    PresetLibraryInventory inventory = ScanPresetLibraryRoot(normalized);
    if (!options_.preferences->SaveRoot(normalized)) {
      worker_result.result.status  = Status::kPreferenceError;
      worker_result.result.message = "The preset folder setting cannot be saved.";
      return worker_result;
    }
    worker_root_            = normalized;
    worker_result.root      = normalized;
    worker_result.inventory = std::move(inventory);
    return worker_result;
  });
}

auto PresetLibraryService::Save(const AdjustmentTransferPackage& package, std::string_view name)
    -> Status {
  if (auto error = PresetNameError(name); !error.empty()) {
    return Refuse(Operation::kSave, Status::kInvalidRequest, std::move(error));
  }
  auto preset = PresetForWrite(Operation::kSave, package);
  if (!preset) {
    return last_result_.status;
  }
  Submit(Operation::kSave, [this, preset, name = std::string{name}] {
    WorkerResult worker_result;
    std::string  error;
    const auto   free_name = FreePresetName(worker_root_, name, &error);
    if (!free_name.has_value()) {
      worker_result.result.status  = Status::kConflict;
      worker_result.result.message = std::move(error);
      return worker_result;
    }
    const fs::path file_name = PresetFileNameFor(*free_name);
    const auto     written   = WritePresetFile(*preset, worker_root_ / file_name);
    if (!written.Ok()) {
      worker_result.result.status  = StatusForRule(written.rule);
      worker_result.result.message = written.message;
      return worker_result;
    }
    worker_result.result.affected_ids.push_back(Utf8(file_name));
    ScanWorkerRoot(&worker_result);
    return worker_result;
  });
  return Status::kOk;
}

auto PresetLibraryService::Export(const AdjustmentTransferPackage& package, const fs::path& path)
    -> Status {
  if (path.empty() || !path.is_absolute() || !path.has_filename()) {
    return Refuse(Operation::kExport, Status::kInvalidRequest, "No export file is selected.");
  }
  auto preset = PresetForWrite(Operation::kExport, package);
  if (!preset) {
    return last_result_.status;
  }
  Submit(Operation::kExport, [this, preset, path] {
    WorkerResult worker_result;
    const auto   written = WritePresetFile(*preset, path, PresetFileWriteMode::kReplace);
    if (!written.Ok()) {
      worker_result.result.status  = StatusForRule(written.rule);
      worker_result.result.message = written.message;
      return worker_result;
    }
    // A file written into the preset folder is a preset of the list.
    const auto relative = path.parent_path().lexically_normal().lexically_relative(
        worker_root_.lexically_normal());
    if (!relative.empty() && *relative.begin() != "..") {
      ScanWorkerRoot(&worker_result);
    }
    return worker_result;
  });
  return Status::kOk;
}

auto PresetLibraryService::PresetForWrite(Operation operation,
                                          const AdjustmentTransferPackage& package)
    -> std::shared_ptr<const AdjustmentTransferPackage> {
  // The written preset is a new value: the package with library references in place of paths.
  auto preset = std::make_shared<AdjustmentTransferPackage>(package);
  try {
    for (auto& grade : preset->color_grades_) {
      if (auto error = ReplaceLutFilePaths(&grade.adjustments,
                                           "the Color Grade '" + grade.display_name + "'",
                                           options_.lut_reference_for_path);
          !error.empty()) {
        Refuse(operation, Status::kRefused, std::move(error));
        return nullptr;
      }
    }
    if (auto error = ReplaceLutFilePaths(&preset->drt_post_.adjustments, "DRT/Post",
                                         options_.lut_reference_for_path);
        !error.empty()) {
      Refuse(operation, Status::kRefused, std::move(error));
      return nullptr;
    }
    preset->fingerprint_ = DocumentTransferFingerprint(*preset);
  } catch (const std::exception& e) {
    Refuse(operation, Status::kRefused, e.what());
    return nullptr;
  }
  if (const auto references = CheckPresetReferences(*preset); !references.Ok()) {
    Refuse(operation, Status::kRefused,
           references.message + ". Save the preset without this item.");
    return nullptr;
  }
  return preset;
}

void PresetLibraryService::Add(std::vector<fs::path> sources) {
  Submit(Operation::kAdd, [this, sources = std::move(sources)] {
    WorkerResult             worker_result;
    std::vector<std::string> failures;
    for (const auto& source : sources) {
      PresetFileReadResult read = ReadPresetFile(source);
      if (!read.Ok()) {
        failures.push_back(read.message);
        continue;
      }
      std::string error;
      const auto  free_name = FreePresetName(worker_root_, Utf8(source.stem()), &error);
      if (!free_name.has_value()) {
        failures.push_back(Utf8(source.filename()) + ": " + error);
        continue;
      }
      const fs::path file_name = PresetFileNameFor(*free_name);
      const auto     written   = WritePresetText(read.canonical_text, worker_root_ / file_name);
      if (!written.Ok()) {
        failures.push_back(Utf8(source.filename()) + ": " + written.message);
        continue;
      }
      worker_result.result.affected_ids.push_back(Utf8(file_name));
    }
    if (!failures.empty()) {
      worker_result.result.status = Status::kRefused;
      for (const auto& failure : failures) {
        if (!worker_result.result.message.empty()) {
          worker_result.result.message += "\n";
        }
        worker_result.result.message += failure;
      }
    }
    ScanWorkerRoot(&worker_result);
    return worker_result;
  });
}

auto PresetLibraryService::Rename(std::string_view id, std::string_view name) -> Status {
  const PresetEntry* entry = FindEntry(id);
  if (entry == nullptr) {
    return Refuse(Operation::kRename, Status::kInvalidRequest,
                  "The preset '" + std::string{id} + "' is not in the preset folder.");
  }
  if (auto error = PresetNameError(name); !error.empty()) {
    return Refuse(Operation::kRename, Status::kInvalidRequest, std::move(error));
  }
  const std::string new_id = entry->group.empty()
                                 ? Utf8(PresetFileNameFor(name))
                                 : entry->group + "/" + Utf8(PresetFileNameFor(name));
  Submit(Operation::kRename, [this, id = std::string{id}, new_id] {
    WorkerResult   worker_result;
    const fs::path source = worker_root_ / PathFromUtf8(id);
    const fs::path target = worker_root_ / PathFromUtf8(new_id);
    if (id != new_id) {
      // QFile::rename refuses an existing target, except the same file in another letter case.
      QFile file(ToQString(source));
      if (!file.rename(ToQString(target))) {
        std::error_code code;
        const bool      exists      = fs::exists(fs::symlink_status(target, code));
        worker_result.result.status = exists ? Status::kConflict : Status::kIoError;
        worker_result.result.message =
            exists ? "A preset with this name exists."
                   : "The preset cannot be renamed: " + file.errorString().toStdString();
        ScanWorkerRoot(&worker_result);
        return worker_result;
      }
    }
    worker_result.result.affected_ids.push_back(new_id);
    ScanWorkerRoot(&worker_result);
    return worker_result;
  });
  return Status::kOk;
}

auto PresetLibraryService::Delete(std::string_view id) -> Status {
  if (FindEntry(id) == nullptr) {
    return Refuse(Operation::kDelete, Status::kInvalidRequest,
                  "The preset '" + std::string{id} + "' is not in the preset folder.");
  }
  Submit(Operation::kDelete, [this, id = std::string{id}] {
    WorkerResult worker_result;
    std::string  error;
    if (!options_.move_to_trash(worker_root_ / PathFromUtf8(id), &error)) {
      worker_result.result.status  = Status::kIoError;
      worker_result.result.message = "The preset cannot be moved to the trash: " + error;
    }
    ScanWorkerRoot(&worker_result);
    return worker_result;
  });
  return Status::kOk;
}

}  // namespace alcedo
