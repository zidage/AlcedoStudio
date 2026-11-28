//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "type/mount_records.hpp"
#include "type/source_file_record.hpp"
#include "type/type.hpp"
#include "utils/file/mount_directory_listing.hpp"
#include "utils/file/source_file_fingerprint.hpp"

namespace alcedo {

/// One mounted file of the mount: its element, its relative path, and its source record.
struct MountScanFileRecord {
  sl_element_id_t  element_id = 0;
  /// Element names from the mount root to the file, joined with `/` (Mounted Folder Plan, P3).
  std::wstring     relative_path;
  SourceFileRecord source;
};

/// One mounted folder below the mount root. The mount root itself is not a folder record.
struct MountScanFolderRecord {
  sl_element_id_t element_id = 0;
  std::wstring    relative_path;

  auto            operator==(const MountScanFolderRecord&) const -> bool = default;
};

/// One ordinary file of the project whose `Image.image_path` is below the root directory. The
/// caller converts the path to a relative path with `/` and names in Unicode NFC, the form of the
/// listing.
struct MountScanOrdinaryFile {
  sl_element_id_t element_id = 0;
  std::wstring    relative_path;
};

/// The stored state of one mount that the scan compares with a listing.
struct MountScanRecords {
  std::vector<MountScanFileRecord>   files          = {};
  std::vector<MountScanFolderRecord> folders        = {};
  /// Adoption candidates (Mounted Folder Plan, D10).
  std::vector<MountScanOrdinaryFile> ordinary_files = {};
};

/// The values of a listed file that the apply step writes into its source record.
struct MountScanSourceValues {
  std::uint64_t         byte_size   = 0;
  std::int64_t          modified_ns = 0;
  SourceFileFingerprint fingerprint;

  auto                  operator==(const MountScanSourceValues&) const -> bool = default;
};

/// A file element that keeps its relative path and gets new source values: changed in place,
/// returned, or adopted.
struct MountScanFileUpdate {
  sl_element_id_t       element_id = 0;
  std::wstring          relative_path;
  MountScanSourceValues values;

  auto                  operator==(const MountScanFileUpdate&) const -> bool = default;
};

/// A file element whose file is at a new relative path. The element stays; its name, its home
/// link, and `Image.image_path` change. The source record becomes present with @ref values.
struct MountScanFileMove {
  sl_element_id_t       element_id = 0;
  std::wstring          previous_relative_path;
  std::wstring          relative_path;
  MountScanSourceValues values;

  auto                  operator==(const MountScanFileMove&) const -> bool = default;
};

/// A listed file that matches no record, with the fingerprint that the comparison read to pair it
/// with a lost record. The apply step imports it by reference.
struct MountScanNewFile {
  std::wstring          relative_path;
  MountScanSourceValues values;

  auto                  operator==(const MountScanNewFile&) const -> bool = default;
};

/// A present record whose file is gone. The source record becomes missing; all other data stays.
struct MountScanMissingFile {
  sl_element_id_t element_id = 0;
  std::wstring    relative_path;

  auto            operator==(const MountScanMissingFile&) const -> bool = default;
};

/**
 * @brief The changes that make one mount equal to its listing (Mounted Folder Plan, section 2.4).
 *
 * A change list is the point-in-time result of one scan; the apply step executes it and drops it.
 * An element id is in one list at most. Each file list is sorted by relative path (the new path
 * of a move). @ref folders_to_add is sorted by relative path, so a parent precedes its children;
 * @ref folders_to_remove is sorted in reverse, so a child precedes its parent.
 */
struct MountScanChangeList {
  std::vector<MountScanFileUpdate>   changed_in_place;
  std::vector<MountScanFileMove>     moved;
  std::vector<MountScanNewFile>      new_files;
  /// Listed files that match no record and whose fingerprint the comparison did not read: no
  /// lost record has their byte size, so no move can end at them. The apply step reads the
  /// fingerprint of each file in the batch that imports it, so a first scan stores its first
  /// photos after one batch of reads. Sorted by relative path.
  std::vector<MountDirectoryFile>    unread_new_files;
  std::vector<MountScanMissingFile>  missing;
  std::vector<MountScanFileUpdate>   returned;
  std::vector<MountScanFileUpdate>   adopted;
  /// Directories that hold a listed file at any depth and have no folder record.
  std::vector<std::wstring>          folders_to_add;
  /// Folder records whose directory holds no listed file, no missing photo, and no record that
  /// the listing could not verify.
  std::vector<MountScanFolderRecord> folders_to_remove;

  /// Present records whose file is at the recorded path with the recorded size and time.
  std::size_t                        unchanged_count                                      = 0;
  /// Missing records that are still missing. They need no write.
  std::size_t                        still_missing_count                                  = 0;
  /// Listed files whose fingerprint could not be read. They are in no list, and their record
  /// stays as it is; the next scan reads them again.
  std::size_t                        unreadable_count                                     = 0;
  /// Records with no listed file below a directory of
  /// MountDirectoryListing::unreadable_directories. Their file can still be there, so they stay
  /// as they are and they match no new path.
  std::size_t                        unverified_count                                     = 0;
  /// Listed files with no record whose fingerprint equals the fingerprint of an unverified
  /// record. They are in no list until a listing can verify that record.
  std::size_t                        deferred_count                                       = 0;

  auto                               operator==(const MountScanChangeList&) const -> bool = default;
};

/// Reads the fingerprint of one listed file. MountService passes a function that calls
/// ComputeSourceFileFingerprint with the root directory plus the relative path.
using MountScanFingerprintFunction =
    std::function<SourceFileFingerprintResult(const MountDirectoryFile& file)>;

/**
 * @brief Compare the stored records of one mount with a listing of its root directory.
 *
 * Rules (Mounted Folder Plan, section 2.4 and MF3):
 * - A file at a recorded path with the recorded byte size and modification time is unchanged. A
 *   different size or time is a change in place; the element stays.
 * - A missing record with a file at its recorded path is returned.
 * - A record with no file at its path is lost. A listed file with no record is a new path. A new
 *   path whose relative path equals the path of an ordinary file is adopted by the ordinary file
 *   with the lowest element id.
 * - A lost record and a new path with one fingerprint make one move. When several records or
 *   paths have one fingerprint, a record takes the first path, in sorted order, with its file
 *   name; the rest pair in sorted order of relative path. A record whose fingerprint version is
 *   not kSourceFileFingerprintVersion matches no path.
 * - The remaining new paths are new files. A remaining lost record that is present becomes
 *   missing; one that is missing stays missing.
 * - The fingerprint function runs only for a listed file whose size or time differs from its
 *   record, for a new path that an ordinary file adopts, and for a new path whose byte size is
 *   the byte size of a lost record with a fingerprint of the current version. The fingerprint
 *   holds the byte size, so a new path with another size is the end of no move; it goes to
 *   MountScanChangeList::unread_new_files without a read. A lost record contributes its stored
 *   fingerprint. An unchanged file is not read.
 * - A listing that is not complete gives an empty change list (Mounted Folder Plan, D5).
 * - @p stop_requested is read before each fingerprint read. When it is true, the function reads
 *   no more files and returns an empty change list; the caller reads the request to tell this
 *   result from a directory with no change.
 *
 * The function reads no storage and no user interface state. It reads file bytes only through
 * @p fingerprint.
 *
 * Thread safety: the function holds no state. It is as safe as @p fingerprint.
 */
[[nodiscard]] auto BuildMountScanChangeList(const MountScanRecords&             records,
                                            const MountDirectoryListing&        listing,
                                            const MountScanFingerprintFunction& fingerprint,
                                            const std::atomic<bool>* stop_requested = nullptr)
    -> MountScanChangeList;

}  // namespace alcedo
