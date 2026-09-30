//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "app/lut_library_inventory.hpp"
#include "utils/lut/lut_library_scan.hpp"

namespace alcedo {

/// File-system steps used by root migration. Each returns an empty string on
/// success or the failure description. The defaults perform real I/O; a caller
/// can substitute one step to reproduce a disk failure at an exact point.
struct LutLibraryFileOperations {
  /// Copy one regular file; must fail when @p to already exists.
  std::function<std::string(const std::filesystem::path& from, const std::filesystem::path& to)>
                                                                copy_file;
  std::function<std::string(const std::filesystem::path& path)> remove_file;
  std::function<std::string(const std::filesystem::path& root, const LutLibraryInventory&)>
      write_inventory;
  std::function<std::string(const std::filesystem::path& root, const LutLibraryUserState&)>
                            write_user_state;

  /// Real file-system implementations of every step.
  [[nodiscard]] static auto Default() -> LutLibraryFileOperations;
};

/// One source file that was copied and verified. Source cleanup deletes the
/// source only while its bytes still hash to @p sha256.
struct LutMigrationCopiedFile {
  std::string   relative_path;
  std::uint64_t size = 0;
  std::string   sha256;
};

/// `lut-migration-cleanup.json` in the new root. It is written into the staging
/// directory before the root switch, so a stop after the switch can resume
/// cleanup from the destination. It is deleted when cleanup completes.
struct LutMigrationCleanupJournal {
  std::string                         source_root;
  std::vector<LutMigrationCopiedFile> files;
};

[[nodiscard]] auto ReadLutMigrationCleanupJournal(const std::filesystem::path& root)
    -> std::optional<LutMigrationCleanupJournal>;

/// Check a destination before any file changes: it must be absent or an empty
/// directory with an existing parent, must not contain or lie inside the source,
/// must not have a leftover staging directory, and its volume must have room for
/// @p required_bytes. Returns an empty string when the destination is usable.
[[nodiscard]] auto ValidateLutLibraryMigrationDestination(const std::filesystem::path& source,
                                                          const std::filesystem::path& destination,
                                                          std::uint64_t required_bytes)
    -> std::string;

struct LutLibraryMigrationPreparation {
  std::string         error;
  bool                canceled          = false;
  /// Files verified in the destination; zero on failure.
  std::size_t         copied_file_count = 0;
  /// The user state written to the destination, including the new previous root.
  LutLibraryUserState user_state;
};

/// Copy the library at @p source into a staging directory beside @p destination,
/// verify every copied byte with SHA-256, write @p inventory, @p user_state (with
/// @p source appended to its previous roots), and the cleanup journal, then
/// rename the staging directory to @p destination.
///
/// Copies every regular file except `.downloads`, the inventory, the user state,
/// and a previous cleanup journal; relative paths are preserved. Links are not
/// followed and are not copied. The source is never modified. On any failure or
/// stop request the staging directory is removed and @p destination is left as
/// it was. The caller persists the root choice afterwards; that write is the
/// commit point. Blocks; must not run on the GUI thread.
[[nodiscard]] auto PrepareLutLibraryMigration(
    const std::filesystem::path& source, const std::filesystem::path& destination,
    const LutLibraryInventory& inventory, LutLibraryUserState user_state,
    const LutLibraryFileOperations& io, std::stop_token stop) -> LutLibraryMigrationPreparation;

/// Remove a destination that PrepareLutLibraryMigration created when the root
/// choice could not be persisted. The previous root stays active.
void DiscardPreparedLutLibraryMigration(const std::filesystem::path& destination);

struct LutLibraryMigrationCleanup {
  std::string              error;
  std::size_t              deleted_file_count = 0;
  /// Source files that changed after copying; they are kept and reported.
  std::vector<std::string> kept_changed_paths;
};

/// Resume or run source cleanup recorded in `<root>/lut-migration-cleanup.json`.
///
/// Deletes each journaled source file only when it still hashes to the copied
/// bytes, then removes directories that became empty. A missing source file
/// counts as already cleaned. On an I/O failure the journal stays so that a
/// later call can resume; after a complete pass the journal is deleted.
/// Blocks; must not run on the GUI thread.
[[nodiscard]] auto CleanLutLibraryMigrationSource(const std::filesystem::path&    root,
                                                  const LutLibraryFileOperations& io,
                                                  std::stop_token                 stop)
    -> LutLibraryMigrationCleanup;

}  // namespace alcedo
