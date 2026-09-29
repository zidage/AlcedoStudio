//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#include "app/lut_library_migration.hpp"

#include <QFile>
#include <QSaveFile>
#include <QString>
#include <algorithm>
#include <cwctype>
#include <json.hpp>
#include <system_error>
#include <utility>

#include "utils/lut/lut_inventory_digest.hpp"

namespace alcedo {
namespace {

namespace fs                              = std::filesystem;
using Json                                = nlohmann::json;

constexpr std::string_view kJournalKind   = "alcedo-lut-migration-cleanup";
constexpr std::string_view kStagingSuffix = ".alcedo-migration";

auto                       ToQString(const fs::path& path) -> QString {
  const std::u8string text = path.u8string();
  return QString::fromUtf8(reinterpret_cast<const char*>(text.data()),
                                                 static_cast<qsizetype>(text.size()));
}

auto WriteBytes(const fs::path& path, const std::string& bytes) -> std::string {
  QSaveFile output(ToQString(path));
  if (!output.open(QIODevice::WriteOnly)) return output.errorString().toStdString();
  if (output.write(bytes.data(), static_cast<qint64>(bytes.size())) !=
      static_cast<qint64>(bytes.size())) {
    std::string error = output.errorString().toStdString();
    output.cancelWriting();
    return error;
  }
  if (!output.commit()) return output.errorString().toStdString();
  return {};
}

/// Absolute, normalized form used only for containment checks.
auto ComparablePath(const fs::path& path) -> fs::path {
  std::error_code error;
  fs::path        absolute = fs::weakly_canonical(fs::absolute(path, error), error);
  if (error) absolute = fs::absolute(path, error).lexically_normal();
  return absolute.lexically_normal();
}

auto SameComponent(const fs::path& left, const fs::path& right) -> bool {
#ifdef _WIN32
  const std::wstring a = left.wstring();
  const std::wstring b = right.wstring();
  return a.size() == b.size() &&
         std::equal(a.begin(), a.end(), b.begin(),
                    [](wchar_t x, wchar_t y) { return std::towlower(x) == std::towlower(y); });
#else
  return left == right;
#endif
}

/// True when @p child equals @p parent or lies below it (case-insensitive on Windows).
auto IsSameOrInside(const fs::path& child, const fs::path& parent) -> bool {
  auto child_it  = child.begin();
  auto parent_it = parent.begin();
  for (; parent_it != parent.end(); ++parent_it, ++child_it) {
    if (parent_it->empty()) continue;
    if (child_it == child.end() || !SameComponent(*child_it, *parent_it)) return false;
  }
  return true;
}

auto StagingPathFor(const fs::path& destination) -> fs::path {
  const fs::path normalized = destination.lexically_normal();
  fs::path       name       = normalized.filename();
  if (name.empty()) name = normalized.parent_path().filename();
  return ComparablePath(destination).parent_path() /
         LutPathFromUtf8("." + LutPathToUtf8(name) + std::string(kStagingSuffix));
}

auto IsLibraryStateFile(const std::string& relative_path) -> bool {
  return relative_path == kLutLibraryInventoryFileName ||
         relative_path == kLutLibraryStateFileName || relative_path == kLutMigrationCleanupFileName;
}

struct SourceTree {
  std::vector<std::string> directories;
  std::vector<std::string> files;
  std::uint64_t            bytes = 0;
  std::string              error;
};

/// Enumerate regular files and directories below @p root without following links.
auto EnumerateSource(const fs::path& root) -> SourceTree {
  SourceTree            tree;
  std::vector<fs::path> pending{fs::path{}};
  while (!pending.empty()) {
    const fs::path relative_dir = std::move(pending.back());
    pending.pop_back();
    std::error_code        error;
    fs::directory_iterator iterator(root / relative_dir, error);
    for (; !error && iterator != fs::directory_iterator{}; iterator.increment(error)) {
      const fs::path  relative = relative_dir / iterator->path().filename();
      std::error_code status_error;
      const auto      status = iterator->symlink_status(status_error);
      if (status_error) {
        tree.error = "cannot read " + LutPathToUtf8(relative) + ": " + status_error.message();
        return tree;
      }
      if (fs::is_symlink(status)) continue;
#ifdef _WIN32
      if (status.type() == fs::file_type::junction) continue;
#endif
      const std::string relative_utf8 = LutPathToUtf8(relative);
      if (fs::is_directory(status)) {
        if (relative.filename() == LutPathFromUtf8(kLutDownloadsDirectoryName)) continue;
        tree.directories.push_back(relative_utf8);
        pending.push_back(relative);
      } else if (fs::is_regular_file(status) && !IsLibraryStateFile(relative_utf8)) {
        tree.files.push_back(relative_utf8);
        tree.bytes += iterator->file_size(status_error);
      }
    }
    if (error) {
      tree.error = "cannot read " + LutPathToUtf8(relative_dir) + ": " + error.message();
      return tree;
    }
  }
  std::sort(tree.directories.begin(), tree.directories.end());
  std::sort(tree.files.begin(), tree.files.end());
  return tree;
}

auto SerializeJournal(const LutMigrationCleanupJournal& journal) -> std::string {
  Json files = Json::array();
  for (const LutMigrationCopiedFile& file : journal.files) {
    files.push_back({{"path", file.relative_path}, {"size", file.size}, {"sha256", file.sha256}});
  }
  return Json{{"schema", 1},
              {"kind", kJournalKind},
              {"source_root", journal.source_root},
              {"files", std::move(files)}}
      .dump(1);
}

}  // namespace

auto LutLibraryFileOperations::Default() -> LutLibraryFileOperations {
  LutLibraryFileOperations io;
  io.copy_file = [](const fs::path& from, const fs::path& to) -> std::string {
    std::error_code error;
    fs::copy_file(from, to, fs::copy_options::none, error);
    return error ? error.message() : std::string{};
  };
  io.remove_file = [](const fs::path& path) -> std::string {
    std::error_code error;
    fs::remove(path, error);
    return error ? error.message() : std::string{};
  };
  io.write_inventory  = WriteLutLibraryInventoryFile;
  io.write_user_state = WriteLutLibraryUserStateFile;
  io.write_package_receipt = WriteLutPackageReceiptFile;
  return io;
}

auto ReadLutMigrationCleanupJournal(const fs::path& root)
    -> std::optional<LutMigrationCleanupJournal> {
  QFile input(ToQString(root / LutPathFromUtf8(kLutMigrationCleanupFileName)));
  if (!input.open(QIODevice::ReadOnly)) return std::nullopt;
  const QByteArray bytes = input.readAll();
  const Json       value = Json::parse(bytes.begin(), bytes.end(), nullptr, false);
  if (value.is_discarded() || !value.is_object() || value.value("schema", 0) != 1 ||
      value.value("kind", std::string{}) != kJournalKind || !value.contains("files") ||
      !value["files"].is_array()) {
    return std::nullopt;
  }
  LutMigrationCleanupJournal journal;
  journal.source_root = value.value("source_root", std::string{});
  for (const Json& item : value["files"]) {
    LutMigrationCopiedFile file{item.value("path", std::string{}),
                                item.value("size", std::uint64_t{0}),
                                item.value("sha256", std::string{})};
    if (!IsSafeLutRelativePath(file.relative_path) || file.sha256.size() != 64) return std::nullopt;
    journal.files.push_back(std::move(file));
  }
  if (journal.source_root.empty()) return std::nullopt;
  return journal;
}

auto ValidateLutLibraryMigrationDestination(const fs::path& source, const fs::path& destination,
                                            std::uint64_t required_bytes) -> std::string {
  if (destination.empty()) return "The destination folder is not set.";
  const fs::path from = ComparablePath(source);
  const fs::path to   = ComparablePath(destination);
  if (IsSameOrInside(to, from)) return "The destination is inside the current LUT library.";
  if (IsSameOrInside(from, to)) return "The current LUT library is inside the destination.";
  std::error_code error;
  if (!fs::is_directory(to.parent_path(), error)) {
    return "The destination's parent folder does not exist.";
  }
  if (fs::exists(to, error)) {
    if (!fs::is_directory(to, error) || !fs::is_empty(to, error) || error) {
      return "The destination already contains files. Choose an empty or new folder.";
    }
  }
  if (fs::exists(StagingPathFor(destination), error)) {
    return "A previous migration left a staging folder: " +
           LutPathToUtf8(StagingPathFor(destination));
  }
  const fs::space_info space = fs::space(to.parent_path(), error);
  if (!error && space.available < required_bytes) {
    return "The destination volume does not have enough free space.";
  }
  return {};
}

auto PrepareLutLibraryMigration(const fs::path& source, const fs::path& destination,
                                const LutLibraryInventory& inventory,
                                LutLibraryUserState user_state, const LutLibraryFileOperations& io,
                                std::stop_token stop) -> LutLibraryMigrationPreparation {
  LutLibraryMigrationPreparation result;
  const SourceTree               tree = EnumerateSource(source);
  if (!tree.error.empty()) {
    result.error = tree.error;
    return result;
  }
  if (std::string invalid = ValidateLutLibraryMigrationDestination(source, destination, tree.bytes);
      !invalid.empty()) {
    result.error = std::move(invalid);
    return result;
  }

  const fs::path staging = StagingPathFor(destination);
  const auto     fail    = [&](std::string message) {
    std::error_code ignored;
    fs::remove_all(staging, ignored);
    result.error = std::move(message);
    return result;
  };
  std::error_code error;
  fs::create_directory(staging, error);
  if (error) return fail("The staging folder cannot be created: " + error.message());
  for (const std::string& directory : tree.directories) {
    fs::create_directories(staging / LutPathFromUtf8(directory), error);
    if (error) return fail("The staging folder cannot be created: " + error.message());
  }

  LutMigrationCleanupJournal journal;
  journal.source_root = LutPathToUtf8(ComparablePath(source));
  for (const std::string& relative : tree.files) {
    if (stop.stop_requested()) {
      result.canceled = true;
      return fail("The migration was canceled.");
    }
    const fs::path                   from        = source / LutPathFromUtf8(relative);
    const fs::path                   to          = staging / LutPathFromUtf8(relative);
    const std::optional<LutFileHash> source_hash = HashLutFile(from);
    if (!source_hash) return fail("Cannot read " + relative + ".");
    if (std::string copy_error = io.copy_file(from, to); !copy_error.empty()) {
      return fail("Cannot copy " + relative + ": " + copy_error);
    }
    const std::optional<LutFileHash> copied_hash = HashLutFile(to);
    if (!copied_hash || copied_hash->sha256 != source_hash->sha256 ||
        copied_hash->size != source_hash->size) {
      return fail("The copy of " + relative + " does not match the source.");
    }
    journal.files.push_back({relative, source_hash->size, source_hash->sha256});
  }

  user_state.previous_roots.push_back(journal.source_root);
  if (std::string write_error = io.write_inventory(staging, inventory); !write_error.empty()) {
    return fail("The LUT inventory cannot be written: " + write_error);
  }
  if (std::string write_error = io.write_user_state(staging, user_state); !write_error.empty()) {
    return fail("The LUT library state cannot be written: " + write_error);
  }
  if (std::string write_error = WriteBytes(staging / LutPathFromUtf8(kLutMigrationCleanupFileName),
                                           SerializeJournal(journal));
      !write_error.empty()) {
    return fail("The migration record cannot be written: " + write_error);
  }
  if (stop.stop_requested()) {
    result.canceled = true;
    return fail("The migration was canceled.");
  }

  const fs::path target = ComparablePath(destination);
  if (fs::exists(target, error)) fs::remove(target, error);
  fs::rename(staging, target, error);
  if (error) {
    std::error_code ignored;
    fs::create_directory(target, ignored);
    return fail("The destination folder cannot be created: " + error.message());
  }
  result.copied_file_count = journal.files.size();
  result.user_state        = std::move(user_state);
  return result;
}

void DiscardPreparedLutLibraryMigration(const fs::path& destination) {
  std::error_code error;
  const fs::path  target = ComparablePath(destination);
  fs::remove_all(target, error);
  fs::create_directory(target, error);
}

auto CleanLutLibraryMigrationSource(const fs::path& root, const LutLibraryFileOperations& io,
                                    std::stop_token stop) -> LutLibraryMigrationCleanup {
  LutLibraryMigrationCleanup                      result;
  const std::optional<LutMigrationCleanupJournal> journal = ReadLutMigrationCleanupJournal(root);
  if (!journal) return result;
  const fs::path        source = LutPathFromUtf8(journal->source_root);
  std::vector<fs::path> directories;
  for (const LutMigrationCopiedFile& file : journal->files) {
    if (stop.stop_requested()) {
      result.error = "Source cleanup was stopped; it resumes on the next start.";
      return result;
    }
    const fs::path  path = source / LutPathFromUtf8(file.relative_path);
    std::error_code error;
    for (fs::path parent = LutPathFromUtf8(file.relative_path).parent_path(); !parent.empty();
         parent          = parent.parent_path()) {
      directories.push_back(parent);
    }
    if (!fs::exists(path, error)) continue;
    const std::optional<LutFileHash> hash = HashLutFile(path);
    if (!hash || hash->sha256 != file.sha256 || hash->size != file.size) {
      result.kept_changed_paths.push_back(file.relative_path);
      continue;
    }
    if (std::string remove_error = io.remove_file(path); !remove_error.empty()) {
      result.error =
          "Cannot delete the migrated source file " + file.relative_path + ": " + remove_error;
      return result;
    }
    ++result.deleted_file_count;
  }
  std::sort(
      directories.begin(), directories.end(), [](const fs::path& left, const fs::path& right) {
        return std::distance(left.begin(), left.end()) > std::distance(right.begin(), right.end());
      });
  directories.erase(std::unique(directories.begin(), directories.end()), directories.end());
  for (const fs::path& directory : directories) {
    std::error_code error;
    if (fs::is_empty(source / directory, error) && !error) fs::remove(source / directory, error);
  }
  std::error_code error;
  fs::remove(root / LutPathFromUtf8(kLutMigrationCleanupFileName), error);
  if (error) result.error = "The migration record cannot be deleted: " + error.message();
  return result;
}

}  // namespace alcedo
