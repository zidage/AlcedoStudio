//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "utils/lut/lut_library_scan.hpp"

namespace alcedo {

/// Library root layout
/// (docs/roadmap/alcedo_studio/ui/lut_library_and_package_management_plan.md 4.3).
inline constexpr std::string_view kLutLibraryStateFileName     = "lut-library.json";
inline constexpr std::string_view kLutMigrationCleanupFileName = "lut-migration-cleanup.json";
inline constexpr std::string_view kLutPackagesDirectoryName    = "packages";
inline constexpr std::string_view kLutUserImportDirectoryName  = "user";
inline constexpr std::string_view kLutDownloadsDirectoryName   = ".downloads";
inline constexpr std::string_view kLutPackageReceiptFileName   = "installed.json";

/// Convert between UTF-8 `/` strings and native paths without locale conversion.
[[nodiscard]] auto                LutPathToUtf8(const std::filesystem::path& path) -> std::string;
[[nodiscard]] auto                LutPathFromUtf8(std::string_view utf8) -> std::filesystem::path;

/// User state that the scan does not produce: favorites and prior roots.
///
/// Persisted as `<root>/lut-library.json`, separate from the scan output
/// `lut-inventory.json`, so a favorite change writes a small file and a rescan
/// never rewrites user choices. Entry identity is the root-relative `/` path;
/// root migration preserves relative paths, so favorites stay linked.
struct LutLibraryUserState {
  /// Sorted, unique root-relative paths. A favorite whose file is absent is kept.
  std::vector<std::string> favorite_paths;
  /// Absolute UTF-8 paths of roots this library was migrated from, oldest first.
  /// L4 resolves legacy absolute references through these mappings.
  std::vector<std::string> previous_roots;
};

struct LutLibraryUserStateReadResult {
  std::optional<LutLibraryUserState> state;
  std::string                        error;
};

[[nodiscard]] auto SerializeLutLibraryUserState(const LutLibraryUserState& state) -> std::string;
/// Parse `lut-library.json` bytes; invalid relative paths are rejected.
[[nodiscard]] auto ParseLutLibraryUserState(std::string_view json_bytes)
    -> LutLibraryUserStateReadResult;
/// Read `<root>/lut-library.json`. A missing file yields an empty state, not an error.
[[nodiscard]] auto ReadLutLibraryUserStateFile(const std::filesystem::path& root)
    -> LutLibraryUserStateReadResult;
/// Atomically replace `<root>/lut-library.json`. Returns an empty string on success.
[[nodiscard]] auto WriteLutLibraryUserStateFile(const std::filesystem::path& root,
                                                const LutLibraryUserState&   state) -> std::string;

/// The part of a package activation receipt (`packages/<id>/installed.json`) that
/// the library scan needs: which content directory is active. L3 writes receipts.
struct LutPackageReceipt {
  std::string package_id;
  /// Library-root-relative path of the active content directory,
  /// `packages/<id>/content/<inventory-hash>`.
  std::string content_directory;
};

/// Read every `packages/<id>/installed.json`. An unreadable or invalid receipt
/// produces a `kInvalidPackageReceipt` diagnostic and no content directory, so its
/// package content is not scanned and the inventory reports itself incomplete.
void               ReadLutPackageReceipts(const std::filesystem::path&    root,
                                          std::vector<LutPackageReceipt>* receipts,
                                          std::vector<LutScanDiagnostic>* diagnostics);

/// Scan loose files and only the active content of installed packages.
///
/// Excludes `.downloads`, all of `packages/` except each receipt's active content
/// directory, receipts, and internal metadata. Entries inside an active content
/// directory receive that package's `managed_package_id`; a loose file that
/// declares `origin: alcedo` stays unowned. Blocks; must not run on the GUI thread.
[[nodiscard]] auto ScanLutLibraryRoot(const std::filesystem::path& root, unsigned worker_count = 0)
    -> LutLibraryInventory;

/// Binary search in the path-sorted @p inventory. Returns nullptr when absent.
[[nodiscard]] auto FindLutLibraryEntry(const LutLibraryInventory& inventory,
                                       std::string_view relative_path) -> const LutLibraryEntry*;

/// Relative paths added, removed, or changed (size, hash, header status,
/// dimensions, metadata, or package ownership) between two path-sorted inventories.
[[nodiscard]] auto ChangedLutLibraryEntryPaths(const LutLibraryInventory& before,
                                               const LutLibraryInventory& after)
    -> std::vector<std::string>;

}  // namespace alcedo
