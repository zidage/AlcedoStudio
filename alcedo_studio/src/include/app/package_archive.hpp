//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace alcedo {

/// Size and lowercase hexadecimal SHA-256 of one file.
struct PackageArchiveFile {
  std::uint64_t size = 0;
  std::string   sha256;
};

/// One file that a package inventory lists (archive-relative `/` path).
struct PackageInventoryFile {
  std::string   path;
  std::uint64_t size = 0;
  std::string   sha256;
};

/// Limits of one extraction, taken from the signed descriptor of the package.
struct PackageArchiveLimits {
  /// Maximum total bytes of all extracted files.
  std::uint64_t maximum_bytes = 0;
  /// Maximum number of extracted regular files.
  std::uint64_t maximum_files = 0;
};

/// Result of ExtractPackageArchive.
struct PackageArchiveExtraction {
  std::string                               error;
  bool                                      canceled = false;
  /// Archive-relative path -> size and SHA-256 of every extracted regular file.
  std::map<std::string, PackageArchiveFile> files;
};

/// UTF-8 text (with `/` separators) of @p path, and the path of UTF-8 @p text.
[[nodiscard]] auto PackagePathToUtf8(const std::filesystem::path& path) -> std::string;
[[nodiscard]] auto PackagePathFromUtf8(std::string_view text) -> std::filesystem::path;

/// True for a package-relative path: non-empty `/`-separated segments, no `.` or `..`, no
/// drive or leading slash, no backslash, NUL, or line break.
[[nodiscard]] auto IsSafePackageRelativePath(std::string_view path) -> bool;

/// Stream @p path through SHA-256. Returns std::nullopt when the file cannot be read.
[[nodiscard]] auto HashPackageFile(const std::filesystem::path& path)
    -> std::optional<PackageArchiveFile>;

/// Check that the downloaded archive at @p archive_path has exactly the signed @p size and
/// lowercase hexadecimal @p sha256. Returns an empty text on success, else the reason.
/// Blocks; must not run on the GUI thread.
[[nodiscard]] auto VerifyPackageArchiveFile(const std::filesystem::path& archive_path,
                                            std::uint64_t size, std::string_view sha256)
    -> std::string;

/// Extract the 7z archive at @p archive_path into the new directory @p destination.
///
/// Accepts only regular files and directories with safe relative paths; rejects links,
/// devices, absolute paths, `..`, duplicate or case-colliding destinations, and entries beyond
/// @p limits while streaming. Every file is hashed while it is written. @p destination must
/// not exist; it is created. On failure the caller removes it. Stops early when @p stop is
/// requested (the result is then canceled). Blocks; must not run on the GUI thread.
[[nodiscard]] auto ExtractPackageArchive(const std::filesystem::path& archive_path,
                                         const std::filesystem::path& destination,
                                         const PackageArchiveLimits&  limits,
                                         const std::atomic<bool>& stop) -> PackageArchiveExtraction;

/// Check that @p files holds every file of @p listed with the listed size and SHA-256, and no
/// file that is not listed except @p inventory_file_name (the inventory itself). Returns an
/// empty text on success, else the reason. Pure function.
[[nodiscard]] auto VerifyExtractedPackageFiles(
    const std::vector<PackageInventoryFile>&         listed,
    const std::map<std::string, PackageArchiveFile>& files, std::string_view inventory_file_name)
    -> std::string;

/// Read @p path into @p bytes. False when the file is larger than @p limit or cannot be read.
[[nodiscard]] auto ReadSmallPackageFile(const std::filesystem::path& path, std::uint64_t limit,
                                        std::string* bytes) -> bool;

}  // namespace alcedo
