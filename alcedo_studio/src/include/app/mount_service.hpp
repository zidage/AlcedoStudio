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
#include <optional>
#include <span>
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
#include "utils/file/mount_directory_listing.hpp"
#include "utils/file/volume_identity.hpp"

namespace alcedo {

/// Why a mount operation was refused or failed (Mounted Folder Plan, section 6.2).
enum class MountError : std::uint8_t {
  /// The directory is inside a mounted directory, or contains one (D12).
  kNestedMount,
  /// The directory does not exist, or is not a directory.
  kDirectoryUnreachable,
  /// No mount has the id.
  kUnknownMount,
  /// A storage write failed.
  kStorageFailure,
  /// The element is not a mounted photo whose file is missing (Locate).
  kPhotoIsNotMissing,
  /// The file is not below the root directory of the mount of the photo (Locate).
  kFileOutsideMount,
  /// The file is below the root directory, but a scan does not list it: an unsupported type, a
  /// name or a directory that starts with a period, or a link on the way (Locate).
  kFileIsNotListed,
  /// Another photo of the mount has the file as its present source (Locate).
  kFileHasPhoto,
  /// The file could not be read (Locate).
  kFileUnreadable,
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

/// One mount as ListMounts reads it. The Disk section of the Collections panel groups the
/// mounts by `record.volume_identity`, and a mount with an empty identity by
/// @ref volume_mount_point (#1076).
struct MountSummary {
  /// The stored mount. `record.volume_name` is the last read name of the volume; it stays while
  /// the volume is offline.
  MountRootRecord       record;
  MountReachability     reachability = MountReachability::kNotScanned;
  /// The mount point of the volume (a drive such as `E:\` on Windows, the mount path on macOS
  /// and Linux), as the last scan of this session read it. Empty while @ref reachability is not
  /// kOnline.
  std::filesystem::path volume_mount_point;
  /// Mounted files whose source record is present.
  std::size_t       present_count = 0;
  /// Mounted files whose source record is missing.
  std::size_t       missing_count = 0;
  /// The mounted files whose source record is present, in ascending element id order. The
  /// Library disables Remove from Library for them (Mounted Folder Plan, section 2.3).
  std::vector<sl_element_id_t> present_element_ids = {};
  /// The mounted files whose source record is missing, in ascending element id order. Their
  /// tiles show the missing state.
  std::vector<sl_element_id_t> missing_element_ids = {};
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
  /// Listed files whose fingerprint could not be read, in the comparison (see
  /// MountScanChangeList) or in a new file batch. They stay as they are for the next scan.
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
  /// Each directory below the root directory that the listing of this scan found, as a relative
  /// path, without the root itself. Empty for kOffline and for a listing that the stop request
  /// ended. The directory watcher of the scan triggers registers these (Mounted Folder Plan,
  /// MF7).
  std::vector<std::wstring> listed_directories;
  /// The new root directory when the scan found the recorded volume under a new mount point
  /// (MountScanRequest::find_moved_volume) and relocated the root before the listing.
  std::optional<std::filesystem::path> relocated_root;
  /// True when the scan reached the volume and stored its new display name (#1088).
  bool                                 volume_name_updated = false;
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
  /// Optional. The scan reads it for each directory of the listing, before each fingerprint
  /// read, and before each new file batch. A batch that started its import completes, so a stop
  /// leaves each batch stored in full or not started.
  const std::atomic<bool>*                      stop_requested = nullptr;
  /// Optional. Called after the first batch for each file that changed in place, moved, or
  /// returned, so the thumbnail owner reads the file again.
  std::function<void(sl_element_id_t)>          invalidate_thumbnail;
  /// Optional. Called after the first batch and after each new file batch.
  std::function<void(const MountScanProgress&)> on_progress;
  std::size_t                                   new_file_batch_size = kMountScanNewFileBatchSize;
  /// When the root directory is unreachable and the mount has a volume identity, look for that
  /// volume under another mount point and relocate the root to the same path below it
  /// (Mounted Folder Plan, MF8). The scan of the project open sets it.
  bool                                          find_moved_volume   = false;
};

/// The mounted volumes for the volume search of MountScanRequest::find_moved_volume.
/// ListVolumeMountPoints in production; a test injects a list.
using VolumeMountPointList = std::function<std::vector<VolumeMountPoint>()>;

/// The volume identity of a directory in the form of ReadVolumeIdentity. ReadVolumeIdentity in
/// production; a test injects an identity.
using VolumeIdentityReader = std::function<std::string(const std::filesystem::path&)>;

/// The display name and the mount point of the volume of a directory. ReadVolumeLabel in
/// production; a test injects a label.
using VolumeLabelReader    = std::function<VolumeLabel(const std::filesystem::path&)>;

/// Removes the edit data (pipelines and edit history) of the photos that RemoveMissingPhotos
/// removed. PipelineMgmtService::DeletePipelines in production; MountService cannot depend on
/// PipelineMgmtService, which depends on ProjectService.
using MountEditDataRemoval = std::function<void(std::span<const sl_element_id_t>)>;

/// The photos that RemoveMissingPhotos removed.
struct MountMissingPhotoRemoval {
  std::vector<sl_element_id_t> element_ids;
  /// The image of each element of @ref element_ids, at the same index; 0 when it had none.
  std::vector<image_id_t>      image_ids;
};

/**
 * @brief The mounts of one project: create, scan, and remove (Mounted Folder Plan, MF4).
 *
 * @details A mount binds a new collection to a disk directory. The collection is always a child of
 * the library root: an ordinary collection never holds a mount (#1076), so the Collections
 * section holds the rows that the user arranges and the Disk section the rows that follow the
 * disk. A scan makes the collection equal
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
  /// @param list_volumes The mounted volumes; empty takes ListVolumeMountPoints.
  /// @param read_volume_identity The volume identity of a directory; empty takes
  ///        ReadVolumeIdentity.
  /// @param read_volume_label The name and the mount point of the volume of a directory; empty
  ///        takes ReadVolumeLabel.
  /// @throws std::invalid_argument when a service is null.
  MountService(std::shared_ptr<Storage> storage, std::shared_ptr<SleeveServiceImpl> sleeve_service,
               std::shared_ptr<ImagePoolService> image_pool_service,
               VolumeMountPointList              list_volumes         = {},
               VolumeIdentityReader              read_volume_identity = {},
               VolumeLabelReader                 read_volume_label    = {});

  /**
   * @brief Bind a new collection in the library root to @p directory.
   *
   * The collection takes the name of the directory. The stored root directory is the canonical
   * path (`std::filesystem::canonical`), with the identity and the display name of its volume.
   * The first scan is a separate ScanMount call.
   *
   * @return The id of the new mount.
   * @throws MountOperationError with kDirectoryUnreachable, kNestedMount (the directory is, is
   *         inside, or contains a mounted directory), or kStorageFailure. A refused or failed
   *         call changes nothing.
   */
  auto CreateMount(const std::filesystem::path& directory) -> mount_id_t;

  /**
   * @brief List the root directory of a mount, compare it with the stored records, and apply the
   *        changes (Mounted Folder Plan, sections 2.4 and 2.6).
   *
   * Batch 1 holds the folder changes, the moved, adopted, changed, returned, and missing files.
   * Each later batch imports MountScanRequest::new_file_batch_size new files. Each batch is
   * stored before the next one starts.
   *
   * The comparison reads the fingerprint of a new file only when a lost record can pair with it
   * (BuildMountScanChangeList). Each other new file is read in the batch that imports it, so the
   * first scan of a directory stores its first photos after one batch of reads. A stop request
   * ends the scan within one fingerprint read plus the import of the current batch.
   *
   * The volume check comes before the listing: when the mount has a recorded volume identity
   * and the root directory has another identity (or none), the root is on another volume, and
   * the mount is offline (D5). Production case: on Linux or macOS the root is the mount point
   * of a volume, and after the volume is unmounted the empty mount point directory stays. A
   * listing of that directory would make each photo of the mount missing.
   *
   * A scan that reaches the volume stores the display name of the volume when it changed
   * (MountRootRecord::volume_name), before batch 1, and keeps its mount point for ListMounts.
   *
   * @return kOffline with no change when the root directory is unreachable or on another volume;
   *         kCanceled when the stop request ended the scan; kStorageFailure when a batch or the
   *         write of the volume name failed (MountScanResult::failed_batch is 0 for the name).
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

  /**
   * @brief Bind the mount @p mount_id to the new location @p directory of its root directory
   *        (Locate Folder; Mounted Folder Plan, MF8).
   *
   * The mount keeps each element. The stored root directory becomes the canonical path of
   * @p directory, the volume identity is read again, and the `Image.image_path` of each file of
   * the mount moves from the old root to the new root. The next scan compares the new location:
   * a file that is not there becomes missing, and a new file is imported.
   *
   * Write order: the MountRoot row, then `Image.image_path`. When the second write fails, the
   * next scan finds each file at the new location by its fingerprint (a move) and completes the
   * relocation.
   *
   * @throws MountOperationError with kUnknownMount, kDirectoryUnreachable, kNestedMount (the
   *         directory is, is inside, or contains the directory of another mount), or
   *         kStorageFailure. A refused call changes nothing.
   */
  void RelocateRoot(mount_id_t mount_id, const std::filesystem::path& directory);

  /**
   * @brief Take @p file as the source of the missing mounted photo @p element_id (Locate;
   *        Mounted Folder Plan, MF8).
   *
   * The file must be below the root directory of the mount of the photo, be a file that a scan
   * lists, and be the present source of no other photo of the mount. The photo keeps its
   * element, edit history, marks, and collection links. It gets the file name, the home link in
   * the folder of the file's directory (a missing folder is added), `Image.image_path`, and the
   * source values of the file; the fingerprint can be different from the record.
   *
   * @throws MountOperationError with kPhotoIsNotMissing, kFileOutsideMount, kFileIsNotListed,
   *         kFileHasPhoto, kFileUnreadable, or kStorageFailure. A refused call changes nothing.
   */
  void LocateMissingFile(sl_element_id_t element_id, const std::filesystem::path& file);

  /**
   * @brief Remove each missing photo of the mount @p mount_id from the project: the element,
   *        its links, its source record, its Image row, and through @p remove_edit_data its
   *        pipelines and edit history (Remove Missing Photos; Mounted Folder Plan, MF8).
   *
   * This is the only removal of mounted photos from the project. It removes no file from the
   * disk. A present photo stays.
   *
   * @return The removed photos; empty when the mount has no missing photo.
   * @throws MountOperationError with kUnknownMount, or kStorageFailure when the sleeve write
   *         failed (nothing is removed then).
   */
  auto RemoveMissingPhotos(mount_id_t mount_id, const MountEditDataRemoval& remove_edit_data)
      -> MountMissingPhotoRemoval;

  /// Every mount in ascending id order, with its reachability, the mount point of its volume,
  /// and its file counts.
  auto ListMounts() const -> std::vector<MountSummary>;

 private:
  /// What this session knows about a mount: the state of its last scan and, while it is
  /// kOnline, the mount point of its volume.
  struct MountSessionState {
    MountReachability     reachability = MountReachability::kNotScanned;
    std::filesystem::path volume_mount_point;
  };

  auto FindMount(mount_id_t mount_id) const -> MountRootRecord;
  /// Set a state other than kOnline; the mount point is cleared.
  void SetReachability(mount_id_t mount_id, MountReachability reachability);
  /// Set kOnline with @p mount_point as the mount point of the volume.
  void SetOnline(mount_id_t mount_id, std::filesystem::path mount_point);
  /// Read the label of the volume of the root of @p mount, set the mount kOnline with the mount
  /// point, and store the name when it differs from `mount.volume_name`.
  /// @return True when the name was stored.
  /// @throws std::runtime_error when the name write fails; the mount is kOnline then.
  auto RecordVolumeReached(const MountRootRecord& mount) -> bool;
  /// RelocateRoot with the operation lock held. Returns the stored canonical root.
  auto RelocateRootLocked(const MountRootRecord& mount, const std::filesystem::path& directory)
      -> std::filesystem::path;
  /// The same path as the root of @p mount below a mount point of its recorded volume; empty
  /// when no listed volume has the identity or no such directory exists.
  auto FindRootOnMovedVolume(const MountRootRecord& mount) const
      -> std::optional<std::filesystem::path>;
  /// The listing of the root of @p mount, or a kRootUnreachable listing when the root is on
  /// another volume than the recorded one.
  auto ListRootOnRecordedVolume(const MountRootRecord&   mount,
                                const std::atomic<bool>* stop_requested) const
      -> MountDirectoryListing;

  std::shared_ptr<Storage>                          storage_;
  std::shared_ptr<SleeveServiceImpl>                sleeve_service_;
  std::shared_ptr<ImagePoolService>                 image_pool_service_;
  VolumeMountPointList                              list_volumes_;
  VolumeIdentityReader                              read_volume_identity_;
  VolumeLabelReader                                 read_volume_label_;

  /// Serializes CreateMount, ScanMount, RemoveMount, RelocateRoot, LocateMissingFile, and
  /// RemoveMissingPhotos.
  std::mutex                                        operation_mutex_;
  mutable std::mutex                                session_mutex_;
  std::unordered_map<mount_id_t, MountSessionState> session_states_;
};

}  // namespace alcedo
