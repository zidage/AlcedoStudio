//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace alcedo {

/// How ListMountDirectory ended.
enum class MountDirectoryListingStatus : std::uint8_t {
  /// Every readable directory below the root is in the listing.
  kComplete = 0,
  /// The root does not exist, is not a directory, or cannot be opened. The listing is empty, and
  /// it does not mean that the directory is empty.
  kRootUnreachable,
  /// The stop request was set. The listing is empty.
  kStopped,
};

/// One supported image file of a listing.
struct MountDirectoryFile {
  /// Path from the root: element names joined with `/`, each name in Unicode NFC.
  std::wstring  relative_path;
  std::uint64_t byte_size                                           = 0;
  /// Modification time of the file in nanoseconds since the Unix epoch.
  std::int64_t  modified_ns                                         = 0;

  auto          operator==(const MountDirectoryFile&) const -> bool = default;
};

/**
 * @brief The directories and supported image files below a root directory at one time.
 *
 * A listing is the point-in-time input of one scan (Mounted Folder Plan, section 5.2). The scan
 * owns it and drops it when the scan ends. Each list is sorted by relative path.
 */
struct MountDirectoryListing {
  MountDirectoryListingStatus     status = MountDirectoryListingStatus::kComplete;
  /// Each directory below the root, without the root itself.
  std::vector<std::wstring>       directories;
  std::vector<MountDirectoryFile> files;
  /// Directories that could not be opened, or that hold an entry whose type or attributes could
  /// not be read. Their entries can be absent from @ref files and @ref directories. An empty
  /// path names the root.
  std::vector<std::wstring>       unreadable_directories;

  [[nodiscard]] auto              IsComplete() const -> bool {
    return status == MountDirectoryListingStatus::kComplete;
  }
};

/**
 * @brief List the directories and the supported image files below @p root.
 *
 * Rules (Mounted Folder Plan, section 2.4):
 * - A file is listed when it is a regular file whose extension has an import category
 *   (`CategoryForPath` is not `ImportFileCategory::Other`). This is the file type rule of the
 *   folder import.
 * - The listing does not follow a symbolic link or a junction, and it does not enter or list an
 *   entry whose name starts with a period.
 * - The listing reads directory entries and file attributes only. It opens no file.
 *
 * @param root           Root directory of the mount.
 * @param stop_requested Optional stop request; the listing checks it before each directory.
 *
 * Thread safety: the function holds no state; any thread can call it. It is slow for a large
 * tree, so call it from a worker thread.
 */
[[nodiscard]] auto ListMountDirectory(const std::filesystem::path& root,
                                      const std::atomic<bool>*     stop_requested = nullptr)
    -> MountDirectoryListing;

/**
 * @brief True when @p path is @p root or a path below it.
 *
 * The comparison is lexical and reads no file system: both paths are normalized
 * (`lexically_normal`, no empty last element) and compared element by element, so `D:/Photos2`
 * is not below `D:/Photos`. Windows compares the elements without case (ordinal, like NTFS);
 * the other platforms compare them exactly.
 */
[[nodiscard]] auto IsPathAtOrBelow(const std::filesystem::path& path,
                                   const std::filesystem::path& root) -> bool;

/**
 * @brief The relative path of @p path below @p root in the form of a listing: the elements
 *        after @p root joined with `/`, each name in Unicode NFC.
 *
 * The root elements compare as in IsPathAtOrBelow; the elements below the root keep their
 * spelling. Reads no file system.
 *
 * @return std::nullopt when @p path is not strictly below @p root.
 */
[[nodiscard]] auto MountRelativePathOf(const std::filesystem::path& root,
                                       const std::filesystem::path& path)
    -> std::optional<std::wstring>;

/**
 * @brief The entry that ListMountDirectory(@p root) gives for the one file @p path.
 *
 * Locate of a missing photo uses it, so the located file gets the values that a scan stores
 * (Mounted Folder Plan, MF8). The rules of the listing apply: below @p root, no element whose
 * name starts with a period, no symbolic link or junction on the way, a regular file with an
 * import category.
 *
 * @return std::nullopt when the listing of @p root does not list @p path, or when its
 *         attributes cannot be read.
 */
[[nodiscard]] auto ReadMountDirectoryFile(const std::filesystem::path& root,
                                          const std::filesystem::path& path)
    -> std::optional<MountDirectoryFile>;

}  // namespace alcedo
