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

#include "utils/lut/lut_metadata.hpp"

namespace alcedo {

/// File name of the persisted local inventory in the library root.
inline constexpr std::string_view kLutLibraryInventoryFileName = "lut-inventory.json";

/// One `.cube` file found by a library scan.
///
/// `name` is the file stem and `relative_path` the root-relative `/` path; the
/// render pipeline refers to the file path, so equal names in different folders
/// are separate entries. `sha256` is set only for files that declare
/// `origin: alcedo`; user LUTs are never hashed.
struct LutLibraryEntry {
  std::string        name;
  std::string        relative_path;
  std::uint64_t      size         = 0;
  LutHeaderError     header_error = LutHeaderError::kNone;
  std::string        header_message;
  LutHeader          header;
  std::string        sha256;

  [[nodiscard]] auto IsOfficial() const -> bool {
    return header_error == LutHeaderError::kNone && header.Origin() == LutOrigin::kAlcedo;
  }
  [[nodiscard]] auto DisplayName() const -> std::string { return LutDisplayName(header, name); }
  [[nodiscard]] auto PrintOptionName() const -> std::string { return LutPrintOptionName(header); }
};

enum class LutScanDiagnosticKind { kSkippedLink, kUnreadableDirectory, kUnreadableFile };

struct LutScanDiagnostic {
  LutScanDiagnosticKind kind = LutScanDiagnosticKind::kSkippedLink;
  std::string           relative_path;
  std::string           message;
};

/// Result of one scan: every discovered file with its classification, sorted by
/// relative path bytes, plus the subtrees that could not be scanned.
struct LutLibraryInventory {
  std::vector<LutLibraryEntry>   entries;
  std::vector<LutScanDiagnostic> diagnostics;

  /// False when a directory or an official file could not be read. A partial
  /// scan must not be reported as a complete package integrity result.
  [[nodiscard]] auto             Complete() const -> bool;
};

struct LutLibraryScanOptions {
  /// Directory names skipped at any depth, compared exactly.
  std::vector<std::string> excluded_directory_names = {".downloads"};
  /// Parallel header/hash workers. 0 selects the hardware thread count, capped at 8.
  unsigned                 worker_count             = 0;
};

/// Scan @p root recursively for `.cube` files (extension matched case-insensitively).
///
/// Enumeration is serial and does not follow symbolic links or junctions (they are
/// reported as skipped). Header reads and SHA-256 hashing of official files run on
/// a bounded set of worker threads that each take the next unprocessed file; the
/// call blocks until all workers finish. Must not run on the GUI thread.
/// The result is deterministic for a given tree regardless of the worker count.
[[nodiscard]] auto ScanLutLibrary(const std::filesystem::path& root,
                                  const LutLibraryScanOptions& options = {}) -> LutLibraryInventory;

/// Serialize @p inventory as `lut-inventory.json` (schema 1).
[[nodiscard]] auto SerializeLutLibraryInventory(const LutLibraryInventory& inventory)
    -> std::string;

struct LutLibraryInventoryParseResult {
  std::optional<LutLibraryInventory> inventory;
  std::string                        error;

  [[nodiscard]] explicit             operator bool() const { return inventory.has_value(); }
};

/// Parse `lut-inventory.json` bytes. Paths and metadata are validated again, so a
/// damaged or hand-edited file is rejected instead of trusted.
[[nodiscard]] auto ParseLutLibraryInventory(std::string_view json_bytes)
    -> LutLibraryInventoryParseResult;

/// Write @p inventory to `<root>/lut-inventory.json` through a temporary file and
/// rename, so readers see either the previous or the complete new inventory.
/// Returns an empty string on success or the failure description.
[[nodiscard]] auto WriteLutLibraryInventoryFile(const std::filesystem::path& root,
                                                const LutLibraryInventory&   inventory)
    -> std::string;

/// Read and parse `<root>/lut-inventory.json` without scanning the library.
[[nodiscard]] auto ReadLutLibraryInventoryFile(const std::filesystem::path& root)
    -> LutLibraryInventoryParseResult;

}  // namespace alcedo
