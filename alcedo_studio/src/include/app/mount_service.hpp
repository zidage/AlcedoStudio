//  Copyright 2026 Yurun Zi
//  SPDX-License-Identifier: GPL-3.0-only
//  Additional permission under GPLv3 section 7 applies; see the LICENSE file.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "app/image_pool_service.hpp"
#include "app/mount_file_importer.hpp"
#include "app/sleeve_service.hpp"
#include "sleeve/storage.hpp"
#include "type/mount_records.hpp"
#include "type/type.hpp"

namespace alcedo {

/// Why a mount operation was refused or failed (Mounted Folder Plan, section 6.2).
enum class MountError : std::uint8_t {
  /// The directory is inside a mounted directory, or contains one (D12).
  kNestedMount,
  /// The parent folder of a new mount is a mounted folder (P4).
  kParentIsMounted,
  /// The parent folder of a new mount is not a live folder of the library.
  kParentIsNotAFolder,
  /// The directory does not exist, or is not a directory.
  kDirectoryUnreachable,
  /// No mount has the id.
  kUnknownMount,
  /// A storage write failed.
  kStorageFailure,
};

/// The error of a refused or failed mount operation.
class MountOperationError : public std::runtime_error {
 public:
  MountOperationError(MountError code, const std::string& message)
      : std::runtime_error(message), code_(code) {}

  [[nodiscard]] auto Code() const -> MountError { return code_; }

 private:
  MountError code_;
};

/// Whether the root directory of a mount was reachable at its last scan of this session.
enum class MountReachability : std::uint8_t {
  /// No scan of the mount ran since the project opened.
  kNotScanned,
  kOnline,
  /// The last scan found the root directory unreachable (D5).
  kOffline,
};

/// One mount as ListMounts reads it.
struct MountSummary {
  MountRootRecord   record;
  MountReachability reachability  = MountReachability::kNotScanned;
  /// Mounted files whose source record is present.
  std::size_t       present_count = 0;
  /// Mounted files whose source record is missing.
  std::size_t       missing_count = 0;
};

/// How a scan ended.
enum class MountScanStatus : std::uint8_t {
  /// Each change of the listing is stored.
  kComplete,
  /// The root directory is unreachable. Nothing changed (D5).
  kOffline,
  /// The stop request ended the scan. The completed batches stay; the next scan finds the rest.
  kCanceled,
  /// A storage write failed in the batch MountScanResult::failed_batch. The completed batches
  /// stay; the next scan computes the remaining changes again.
  kStorageFailure,
};

/// The result of one scan. The counts are those of the stored batches.
struct MountScanResult {
  MountScanStatus status                 = MountScanStatus::kComplete;
  std::size_t     imported_count         = 0;
  std::size_t     import_failed_count    = 0;
  std::size_t     moved_count            = 0;
  std::size_t     changed_in_place_count = 0;
  std::size_t     missing_count          = 0;
  std::size_t     returned_count         = 0;
  std::size_t     adopted_count          = 0;
  std::size_t     folders_added_count    = 0;
  std::size_t     folders_removed_count  = 0;
  std::size_t     unchanged_count        = 0;
  std::size_t     still_missing_count    = 0;
  /// Listed files whose fingerprint could not be read; see MountScanChangeList.
  std::size_t     unreadable_count       = 0;
  std::size_t     unverified_count       = 0;
  std::size_t     deferred_count         = 0;
  /// Files of the directory whose first and last blocks the scan read.
  std::size_t     fingerprint_read_count = 0;
  /// 1 for the batch of folder and record changes, 2 and up for the batches of new files; 0 when
  /// no batch failed.
  std::size_t     failed_batch           = 0;
  /// The error text of a failed batch.
  std::string     error;
};

/// The progress of the new file batches of one scan.
struct MountScanProgress {
  std::size_t new_files_total = 0;
  std::size_t new_files_done  = 0;
};

/// New files in one batch of a scan (Mounted Folder Plan, section 14).
inline constexpr std::size_t kMountScanNewFileBatchSize = 500;

/// One scan of one mount.
struct MountScanRequest {
  mount_id_t                                    mount_id       = 0;
  /// Imports the new files; required.
  MountFileImporter*                            importer       = nullptr;
  /// Optional. The scan checks it before each new file batch; the current batch completes.
  const std::atomic<bool>*                      stop_requested = nullptr;
  /// Optional. Called after the first batch for each file that changed in place, moved, or
  /// returned, so the thumbnail owner reads the file again.
  std::function<void(sl_element_id_t)>          invalidate_thumbnail;
  /// Optional. Called after the first batch and after each new file batch.
  std::function<void(const MountScanProgress&)> on_progress;
  std::size_t                                   new_file_batch_size = kMountScanNewFileBatchSize;
};

/**
 * @brief The mounts of one project: create, scan, and remove (Mounted Folder Plan, MF4).
 *
 * @details A mount binds a new collection to a disk directory. A scan makes the collection equal
 * to the directory in one direction, from the disk to the library: folders follow the directory
 * tree, a moved file keeps its element, a new file is imported by reference, and a file that is
 * gone becomes missing. The service never writes, renames, moves, or deletes a file of the
 * directory; it opens files only for the fingerprint read (read only, shared).
 *
 * Owners: the sleeve tree through SleeveServiceImpl (the scan entry, one `fs_lock_` scope for
 * each batch), `Image.image_path` through ImagePoolService, and the MountRoot and SourceFile rows
 * through MountStore. ProjectService creates the service with the project and ends it with the
 * project.
 *
 * Write order of a batch: the sleeve tree, then `Image.image_path`, then the source records. The
 * relative path of a mounted file comes from `Image.image_path`, so an interrupted batch leaves a
 * state that the next scan completes (Mounted Folder Plan, MF4 completion record).
 *
 * Thread safety: the mutating operations run one at a time (one mutex). ScanMount blocks for the
 * length of the scan; call it from a worker thread. ListMounts reads storage and does not wait
 * for a scan.
 */
class MountService {
 public:
  /// @throws std::invalid_argument when a service is null.
  MountService(std::shared_ptr<Storage> storage, std::shared_ptr<SleeveServiceImpl> sleeve_service,
               std::shared_ptr<ImagePoolService> image_pool_service);

  /**
   * @brief Bind a new collection in the folder @p parent_folder_id to @p directory.
   *
   * The collection takes the name of the directory. The stored root directory is the canonical
   * path (`std::filesystem::canonical`). The first scan is a separate ScanMount call.
   *
   * @return The id of the new mount.
   * @throws MountOperationError with kDirectoryUnreachable, kParentIsNotAFolder,
   *         kParentIsMounted, kNestedMount (the directory is, is inside, or contains a mounted
   *         directory), or kStorageFailure. A refused or failed call changes nothing.
   */
  auto CreateMount(sl_element_id_t parent_folder_id, const std::filesystem::path& directory)
      -> mount_id_t;

  /**
   * @brief List the root directory of a mount, compare it with the stored records, and apply the
   *        changes (Mounted Folder Plan, sections 2.4 and 2.6).
   *
   * Batch 1 holds the folder changes, the moved, adopted, changed, returned, and missing files.
   * Each later batch imports MountScanRequest::new_file_batch_size new files. Each batch is
   * stored before the next one starts.
   *
   * @return kOffline with no change when the root directory is unreachable; kCanceled when the
   *         stop request ended the scan; kStorageFailure when a batch failed.
   * @throws MountOperationError with kUnknownMount; std::invalid_argument when the request has
   *         no importer.
   */
  auto ScanMount(const MountScanRequest& request) -> MountScanResult;

  /**
   * @brief Remove the mount @p mount_id. Each mounted file becomes an ordinary file with its
   *        current `Image.image_path`, its source record, and all its edits and marks; the
   *        mounted folders and the MountRoot row are removed (D11).
   * @throws MountOperationError with kUnknownMount or kStorageFailure.
   */
  void RemoveMount(mount_id_t mount_id);

  /// Every mount in ascending id order, with its reachability and its file counts.
  auto ListMounts() const -> std::vector<MountSummary>;

 private:
  auto                     FindMount(mount_id_t mount_id) const -> MountRootRecord;
  void                     SetReachability(mount_id_t mount_id, MountReachability reachability);

  std::shared_ptr<Storage> storage_;
  std::shared_ptr<SleeveServiceImpl>                sleeve_service_;
  std::shared_ptr<ImagePoolService>                 image_pool_service_;

  /// Serializes CreateMount, ScanMount, and RemoveMount.
  std::mutex                                        operation_mutex_;
  mutable std::mutex                                reachability_mutex_;
  std::unordered_map<mount_id_t, MountReachability> reachability_;
};

}  // namespace alcedo
