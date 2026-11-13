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

namespace alcedo {

/// One LUT of an official package: stable ID, root-relative `/` path, size, and content hash.
struct LutInventoryRecord {
  std::string   id;
  std::string   relative_path;
  std::uint64_t size = 0;
  /// 64 lowercase hexadecimal characters.
  std::string   sha256;
};

/// A verified non-LUT file of a package (license, attribution, change log).
struct LutAuxiliaryFileRecord {
  std::string   relative_path;
  std::uint64_t size = 0;
  std::string   sha256;
};

struct LutInventoryDigestResult {
  /// 64 lowercase hexadecimal characters; empty on failure.
  std::string        sha256;
  std::string        error;

  [[nodiscard]] auto Ok() const -> bool { return !sha256.empty(); }
};

/// Compute the canonical inventory digest of @p records.
///
/// Encodes `id NUL path NUL size NUL sha256 LF` per record in UTF-8, sorted by
/// path bytes then ID bytes, and hashes the concatenation with SHA-256. Input
/// order does not matter. Rejects NUL/line breaks, absolute or `..` paths,
/// backslashes, invalid hashes, duplicate IDs, and paths that collide after
/// ASCII case folding. Pure function; see docs/lut-package-system.md 2.1.
[[nodiscard]] auto ComputeLutInventoryDigest(std::vector<LutInventoryRecord> records)
    -> LutInventoryDigestResult;

/// Validate a package-relative path: non-empty `/`-separated segments, no `.`/`..`,
/// no drive or leading slash, no backslash, NUL, or line break.
[[nodiscard]] auto IsSafeLutRelativePath(std::string_view path) -> bool;

struct LutFileHash {
  std::string   sha256;
  std::uint64_t size = 0;
};

/// Stream @p path through SHA-256 in 1 MiB blocks and return its lowercase
/// hexadecimal digest and byte size. Returns std::nullopt when the file cannot
/// be read completely. Safe to call concurrently for different files.
[[nodiscard]] auto HashLutFile(const std::filesystem::path& path) -> std::optional<LutFileHash>;

/// Parsed `package-inventory.json` from a package archive (schema 1).
struct LutPackageInventory {
  std::string                         package_id;
  std::string                         revision;
  std::uint64_t                       file_count = 0;
  std::string                         inventory_sha256;
  std::uint64_t                       unpacked_bytes = 0;
  std::vector<LutInventoryRecord>     luts;
  std::vector<LutAuxiliaryFileRecord> auxiliary_files;
};

struct LutPackageInventoryParseResult {
  std::optional<LutPackageInventory> inventory;
  std::string                        error;

  [[nodiscard]] explicit             operator bool() const { return inventory.has_value(); }
};

/// Parse and cross-check `package-inventory.json` bytes.
///
/// Verifies the declared file count, unpacked byte total, and inventory digest
/// against the listed records. It does not read files; archive extraction (L3)
/// compares the listed sizes and hashes with the extracted bytes.
[[nodiscard]] auto ParseLutPackageInventory(std::string_view json_bytes)
    -> LutPackageInventoryParseResult;

}  // namespace alcedo
