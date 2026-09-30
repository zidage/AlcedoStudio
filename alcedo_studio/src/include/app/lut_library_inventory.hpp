//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <cstdint>
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
/// never rewrites user choices. Favorites are stored by entry ID
/// (@ref IsValidLutLibraryEntryId): an official package LUT keeps its favorite across
/// package updates that move its content directory, and a user file keeps it across
/// root migration because relative paths are preserved.
struct LutLibraryUserState {
  /// Sorted, unique entry IDs. A favorite whose entry is absent is kept.
  std::vector<std::string> favorite_entry_ids;
  /// Root-relative favorite paths from files written before entry IDs (the `favorites`
  /// key). The service converts them to entry IDs against the published inventory and
  /// then clears this list; until then they are written back unchanged.
  std::vector<std::string> legacy_favorite_paths;
  /// Absolute UTF-8 paths of roots this library was migrated from, oldest first.
  /// L4 resolves legacy absolute references through these mappings.
  std::vector<std::string> previous_roots;
};

struct LutLibraryUserStateReadResult {
  std::optional<LutLibraryUserState> state;
  std::string                        error;
};

/// True for `official:<package id>/<lut id>` with non-empty IDs, or `library:<path>` with a
/// safe root-relative path. These are the texts DescribeLutReference produces for the
/// reference that selects a library entry (LutLibraryPublication::EntryIdOf).
[[nodiscard]] auto IsValidLutLibraryEntryId(std::string_view entry_id) -> bool;

[[nodiscard]] auto SerializeLutLibraryUserState(const LutLibraryUserState& state) -> std::string;
/// Parse `lut-library.json` bytes; invalid entry IDs and relative paths are rejected.
[[nodiscard]] auto ParseLutLibraryUserState(std::string_view json_bytes)
    -> LutLibraryUserStateReadResult;
/// Read `<root>/lut-library.json`. A missing file yields an empty state, not an error.
[[nodiscard]] auto ReadLutLibraryUserStateFile(const std::filesystem::path& root)
    -> LutLibraryUserStateReadResult;
/// Atomically replace `<root>/lut-library.json`. Returns an empty string on success.
[[nodiscard]] auto WriteLutLibraryUserStateFile(const std::filesystem::path& root,
                                                const LutLibraryUserState&   state) -> std::string;

/// Package activation receipt (`packages/<id>/installed.json`).
///
/// The receipt is the persistent commit point of a package installation: it
/// names the active content directory and records the verified feed descriptor
/// of that content. It does not copy the inventory rows. Receipts written before
/// L3 hold only the package ID and content directory; their descriptor fields
/// stay empty or zero.
struct LutPackageReceipt {
  std::string   package_id;
  /// Library-root-relative path of the active content directory,
  /// `packages/<id>/content/<name>`.
  std::string   content_directory;
  std::string   revision;
  std::uint64_t file_count = 0;
  /// Canonical inventory digest, 64 lowercase hexadecimal characters.
  std::string   inventory_sha256;
  std::uint64_t unpacked_bytes = 0;
  std::string   artifact_url;
  std::uint64_t artifact_size = 0;
  /// SHA-256 of the exact archive bytes, 64 lowercase hexadecimal characters.
  std::string   artifact_sha256;
  /// Sequence of the signed feed that listed this descriptor.
  std::uint64_t feed_sequence = 0;
};

/// Package IDs usable as a directory name: 1-64 characters of `[a-z0-9_-]`.
[[nodiscard]] auto IsLutPackageId(std::string_view id) -> bool;

/// Read every `packages/<id>/installed.json`. An unreadable or invalid receipt
/// produces a `kInvalidPackageReceipt` diagnostic and no content directory, so its
/// package content is not scanned and the inventory reports itself incomplete.
void               ReadLutPackageReceipts(const std::filesystem::path&    root,
                                          std::vector<LutPackageReceipt>* receipts,
                                          std::vector<LutScanDiagnostic>* diagnostics);

[[nodiscard]] auto SerializeLutPackageReceipt(const LutPackageReceipt& receipt) -> std::string;

/// Atomically replace `<root>/packages/<id>/installed.json` through a temporary
/// file and rename. Readers see the previous or the complete new receipt.
/// Returns an empty string on success or the failure description.
[[nodiscard]] auto WriteLutPackageReceiptFile(const std::filesystem::path& root,
                                              const LutPackageReceipt&     receipt) -> std::string;

/// True when the package ownership in @p inventory agrees with @p receipts:
/// every package-owned entry lies in its package's active content directory, and
/// every receipt that declares LUTs has at least one entry. A false result means
/// that a receipt changed after the inventory was written (for example, an
/// installation committed and the process stopped before the inventory write).
[[nodiscard]] auto LutInventoryMatchesPackageReceipts(
    const LutLibraryInventory& inventory, const std::vector<LutPackageReceipt>& receipts) -> bool;

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
